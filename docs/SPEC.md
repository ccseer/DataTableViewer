# DataTableViewer Specification

This document provides the authoritative architectural specification and implementation contracts for `DataTableViewer`, covering bounded-memory paging across SQLite databases and CSV/TSV files.

---

## 1. Architectural Foundations & Invariants

### 1.1 Background & Motivation
Prior architectures relied on one-shot materialization:
- **SQLite:** Capped at 10,000 rows (`SELECT ... LIMIT 10001`), closing connections immediately upon return, rendering row counts inaccurate and large tables truncated.
- **CSV/TSV:** Whole-file in-memory reading capped at 64 MiB or 100,000 rows, exhausting physical memory on giant files and crashing or truncating.
- **Client-side sorting/filtering:** Operated strictly over loaded windows rather than true source data.

### 1.2 Core Abstraction: `ITableSource`
All storage engines implement `dtv::core::ITableSource` (pure STL/C++17 in `src/core/table_source.h`):
- **Lifecycle:** Bound to the lifetime of the currently loaded table/file in `SourceWorker`.
- **Navigation via Keyset `PageToken`:** The source positions by keys/offsets stored in `PageToken` (rowids for SQLite, logical byte spans for CSV), never by SQL `OFFSET`.
- **`PageResult`:** Carries decoded row data, an array of per-row `RefetchKey` structures, cell clamping flags, and the `hasMore` indicator.
- **Generations:**
  - `viewGen`: Invalidates work when switching files or tables.
  - `opGen`: Invalidates in-flight navigation and sorting queries within the active view.
  - `copyRequestId`: Identifies asynchronous clipboard refetch operations; only the newest request writes to the system clipboard.

---

## 2. Part 1: SQLite Keyset Paging

### 2.1 Connection Architecture & Lifetime
1. **Main Connection:** Dedicated read-only connection (`SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX`) confined to `SourceWorker`'s background thread.
2. **Count Connection:** An independent read-only connection on a separate `BackgroundThread` executing `SELECT COUNT(*)` asynchronously. Prevents multi-second row counts from blocking immediate page-1 browsing.
3. **Safety & Mapped Reads:** `PRAGMA mmap_size = 0` is strictly enforced to prevent access violations on truncated files, network shares, or cloud placeholders. `PRAGMA cache_size = -8000` (8 MiB).
4. **Cancellation:** Managed through thread-safe `InterruptHandle` invoking `sqlite3_interrupt()`. Progress handlers check generation staleness.

### 2.2 Keyset Pagination (No `OFFSET`)
For a page size $s$ and resolved rowid alias `rid`:
- **First Page:** `SELECT * FROM "table" ORDER BY rid LIMIT s + 1`
- **Next Page:** `SELECT * FROM "table" WHERE rid > :last_rid ORDER BY rid LIMIT s + 1`
- **Previous Page:** `SELECT * FROM "table" WHERE rid < :first_rid ORDER BY rid DESC LIMIT s` (reversed in memory)
- **Last Page:** `SELECT * FROM "table" ORDER BY rid DESC LIMIT k` (reversed in memory, where $k = \text{total} - (\text{pages} - 1) \times s$)
- **Complexity:** $O(\log N + s)$ per page turn, regardless of distance from table start.

### 2.3 Rowid Alias Resolution & DQS Defense
At open time, the source probes `rowid`, `_rowid_`, and `oid` as **bare identifiers** (never double-quoted).
- Under SQLite's default Double-Quoted String misfeature (DQS 3), double-quoting an unresolved identifier silently degrades into a string literal (`"_rowid_"` returns a constant string on `WITHOUT ROWID` tables).
- Bare identifiers fail loudly if unresolved, guaranteeing safe detection.
- Probed aliases are verified against user column names (case-insensitive) to prevent shadowing.
- `WITHOUT ROWID` tables and virtual tables lacking row identity fall back to forward-only or standard offset paging with server-side sorting disabled.

### 2.4 Server-Side Sorting via Temp Ordinal Table
1. **Deterministic Order:** Created via temporary table on first sort:
   `CREATE TEMP TABLE dtv_ord_<seq> (ord INTEGER PRIMARY KEY, rid INTEGER);`
   `INSERT INTO dtv_ord_<seq> (ord, rid) SELECT ROW_NUMBER() OVER (ORDER BY "col" [ASC|DESC], rid [ASC|DESC]), rid FROM main."table";`
2. **Tie-Breaker:** Rowid acts as deterministic tie-breaker for identical values.
3. **Paging via Range Join:** Subsequent sorted page turns perform exact ordinal index range lookups joined against main table:
   `FROM dtv_ord o CROSS JOIN main."table" m ON m.rowid = o.rid WHERE o.ord BETWEEN :start AND :end`
4. **Zero-Cost Total Count:** `sqlite3_changes64()` immediately yields authoritative total rows, enabling the last-page button without waiting for `COUNT(*)`.

---

## 3. Part 2: CSV/TSV Bounded-Memory Paging (Phase C2)

Supersedes legacy in-memory `TableWorker` parsing and eliminates all 64 MiB and 100,000-row caps.

### 3.1 Streaming Scanner (`CsvRecordScanner`)
- **Engine:** Pure STL C++17 chunked state machine with zero whole-file reads.
- **RFC 4180 Compliance:** Supports multiline quoted fields, doubled quotes (`""`), whitespace preservation, and CRLF / LF line endings.
- **Encoding & Delimiters:** Strips UTF-8 BOM (`\xEF\xBB\xBF`) at base offset 0. Auto-detects comma vs. tab delimiter across initial 4 KiB sample.
- **Constraints:** Enforces a hard limit of 256 columns. Evaluates cooperative cancellation callbacks every 64 KiB.

### 3.2 Dense Binary Disk Index (`CsvRecordIndex`)
- **Structure:** 64-byte `CsvIndexHeader` followed by dense, contiguous 16-byte `CsvRecordSpan` entries (`uint64_t start`, `uint64_t end`).
- **Memory Footprint:**
  - RAM Cache: First 65,536 spans cached directly in memory (1 MiB budget).
  - Sliding Block Cache: 65,536-span block cache for subsequent ordinals.
  - Spans beyond RAM are streamed directly to disk via buffered block writes.
- **Crash-Safe File Isolation:** Index files are written to a per-process private temp directory (`dtv_idx_<pid>_<salt>`) using Win32 `CREATE_NEW`, `FILE_ATTRIBUTE_TEMPORARY`, and `FILE_FLAG_DELETE_ON_CLOSE` for immediate OS-level reclamation upon process termination.

### 3.3 Dynamic Memory Bounds & Safety Caps
1. **32 MiB Decoded Page Budget:** Page cell allocation in memory is capped at 32 MiB decoded bytes per page. Dynamic cell clamp:
   $$\text{perCellCap} = \min\left(4096, \max\left(16, \left\lfloor \frac{32 \times 1024 \times 1024}{\text{pageSize} \times \text{pageColumnCount}} \right\rfloor \right)\right)$$
2. **UTF-8 Safe Truncation:** Cells exceeding `perCellCap` are truncated strictly at valid multi-byte UTF-8 sequence boundaries (stepping back at most 3 bytes). Clamped cells are flagged in `PageResult.clamped`.
3. **Schema Freezing:** Column count and types are sampled over the first 200 data records (bounded by 2 MiB read window) and frozen via `core::TypeInferrer`. Truncated sample cells permanently enforce `Type::String` to prevent corrupting numeric columns.
4. **Shared Non-Exclusive Access:** Opened with `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`. Concurrent modifications or truncations are detected via handle identity, size, and timestamp verification (`verifyUnchanged()`), latching an immediate error.

### 3.4 Asynchronous Clipboard Copy Budget
- Cells flagged as clamped are refetched on demand during copy operations.
- Sparse column refetching extracts only selected columns into `RefetchResult`.
- **64 MiB Aggregate Budget:** If total extracted cell content across a copy selection exceeds 64 MiB (`kMaxCopyBudgetBytes`), the copy operation is aborted, invalidating the batch and leaving the system clipboard untouched to prevent out-of-memory crashes or partial data corruption.

---

## 4. Worker Lifecycle & Concurrency (`SourceWorker`)

### 4.1 Polymorphic Dispatch (`SourceOpenDescriptor`)
`SourceWorker` handles both SQLite and CSV through unified descriptors:
```cpp
enum class SourceKind { Sqlite, Csv };
struct SourceOpenDescriptor {
    SourceKind kind;
    QString path;
    QString tableName;
    char delimiter;
};
```
If dynamic source type mismatches `desc.kind`, `m_source` is reconstructed automatically.

### 4.2 Sliced Indexing & Event-Loop Interleaving
- Background indexing runs in discrete 256 KiB slices via `indexSlice()`, re-queuing subsequent slices through `QMetaObject::invokeMethod(Qt::QueuedConnection)`.
- UI interactions (page navigation, cell selection, window resizing) smoothly interleave between slices without UI freezing.
- **Throttled Progress:** Emits `indexProgress(viewGen, totalRows, isComplete, error)` at most once per 100 ms (unless reaching EOF or encountering errors).

### 4.3 Pending Navigation Queue (`PendingPageRequest`)
- When a user navigates to an unindexed page offset, the request is parked in `m_pendingPageRequest`.
- As background slicing covers the requested range or reaches EOF, the pending request is automatically awakened and fulfilled.
- If slicing encounters an I/O or syntax error, parked requests are immediately rejected with an error so the UI exits busy state.
- Bumping `opGen` supersedes pending page requests while allowing background index slicing to continue to EOF. Bumping `viewGen` halts indexing immediately.

---

## 5. UI Presentation & Behavior

### 5.1 PageBar Controls
- Slim bottom bar: `[First] [Previous] "page / total" [Next] [Last]`.
- Single-page files (`total <= pageSize`) automatically hide the pager bar.
- While row count or indexing is running, total displays as `"page / ..."`.

### 5.2 Status Bar Truthfulness
- Displays loaded range (`rows a-b of N`), column count, file size, format name (`CSV`, `TSV`, `SQLite`), engine credit, and load timing.
- **CSV Sorting Indicator:** Full-file sorting is unsupported for paged CSV/TSV. Clicking column headers keeps sort indicators hidden and displays an explanatory message in the status bar: `"Sorting is not supported for paged CSV/TSV files"`.

### 5.3 Filter Scope
- In this phase, filtering via the search bar applies strictly to the current loaded page. The search placeholder explicitly indicates `"Filter current page"`.

---

## 6. Performance Benchmarks & Targets

| Metric | Target | Actual Measured (1,000,000 rows, ~76 MiB) |
|---|---|---|
| Open Latency | < 100 ms | **5 ms** |
| First Page Fetch (500 rows) | < 500 ms | **2 ms** |
| Full 1M Row Indexing | < 2000 ms | **433 ms** (301 slices) |
| Indexed Page Turn Latency | < 50 ms | **< 10 ms** |
| Process Memory Stability | Constant $O(1)$ plateau | **~4 MB private bytes** (flat across 50 page turns) |

---

## 7. Known Limitations

1. **In-Memory Text Spike:** SQLite materializes oversized text during `sqlite3_step` before display clamping can occur.
2. **Current-Page Filter:** Table filtering operates within the active page. Global search is scheduled for later phases.
3. **Sharing Violations on SQLite:** SQLite hold read locks during table preview; external processes attempting to rename or delete the database file receive sharing violations until the table is closed.
4. **Temp Sorter Spilling:** Very large SQLite sorts may spill sort keys to temp store if SQLite compilation limits are reached.
5. **No Cross-Request Snapshot Isolation:** In SQLite, the COUNT connection and main connection run at different times; concurrent file modifications outside Seer can cause slight count discrepancies.
