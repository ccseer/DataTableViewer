# DataTableViewer Specification

## Parser Contract

`SqliteParser` is used for table discovery only: an empty table name returns the
user table names. Selected SQLite tables are loaded by `SqliteTableSource`.
CSV and TSV files are loaded through `CsvFileSource` with streaming disk-backed
offset indexing, bounded-memory page fetching, and dynamic cell clamping.

## Source Lifetime

`SqliteTableSource` owns a read-only SQLite connection and remains alive for a
selected table. `CsvFileSource` owns a read-only file handle and a temporary
disk-backed offset index (`FILE_FLAG_DELETE_ON_CLOSE`), deleted on shutdown.
`SourceWorker` confines all source operations to its worker thread and shuts
the source down before the thread exits. The UI receives immutable page
snapshots and never owns underlying data handles.

## Threading and Cancellation

`viewGen` invalidates work for a different file or table. `opGen` invalidates
navigation and sorting requests within the selected table. Background CSV
indexing runs in queued 256 KiB slices and checks cancellation every 64 KiB.
Copy refetch uses an independent `copyRequestId`; only the newest copy request
may write to the clipboard. Stale worker results are discarded by the UI
generation checks.

## Sorting Capability

Server-side sorting is available only for SQLite objects with a usable rowid.
Paged SQLite sorting uses native SQLite ordering and deterministic rowid ties.
Tables without a supported row identity remain pageable but do not expose
server-side sorting. Full-file sorting is unsupported for paged CSV/TSV; clicking
a column header displays an explanation in the status bar without modifying order.

## UI Behavior

Large SQLite tables and CSV/TSV files use page navigation with a configurable
page size and asynchronous row counts or background index progress. Single-page
files hide the pager bar. Filtering applies to the current page in this release.
Display cells clamp dynamically (bounded at 32 MiB decoded bytes per page) at
valid UTF-8 code point boundaries. Clamped cells are refetched asynchronously
before copy, subject to an aggregate 64 MiB clipboard budget.

## Known Limitations

- Paged SQLite and CSV/TSV filtering is current-page-only.
- Paged CSV/TSV full-file sorting is unsupported.
- Natural-order restoration for paged SQLite is provided by re-entering the
  table; the paging header cycles between ascending and descending order.
- Markdown copy is limited to 1000 selected rows.
