# AGENTS.md - DataTableViewer

Seer plugin for read-only tabular data preview: CSV, TSV, and SQLite.
C++17 - Qt 6.8 - MSVC - CMake 3.16+

**Reference project:** `../DataTreeViewer`

Use DataTreeViewer as the canonical reference for:
- Seer plugin metadata shape in `bin/plugin.json`
- `ViewerBase` lifecycle and `sigCommand(VCT_StateChange, ...)`
- non-blocking worker cancellation with generation checks
- `BackgroundThread` ownership/destruction pattern
- lazy `init()`, theme/DPI hooks, and POST_BUILD metadata copy

---

## Build & Test

This project is intended to use a single `RelWithDebInfo` Ninja build.

```powershell
cmake --preset RelWithDebInfo
cmake --build C:/Users/corey/Dev/build_output/DataTableViewer --target run_all_tests
cmake --build C:/Users/corey/Dev/build_output/DataTableViewer --target datatableviewer
```

Equivalent manual configure:

```powershell
cmake -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_PREFIX_PATH=C:/Users/corey/Dev/Qt/6.8.3/msvc2022_64
cmake --build build --target run_all_tests
cmake --build build --target datatableviewer
```

Enable/disable parser tests with `-DBUILD_TESTS=OFF`.

If command-line builds fail with missing standard headers such as `string` or
`type_traits`, rerun from a Visual Studio Developer PowerShell or another shell
with the full MSVC environment loaded. That is an environment issue, not a
project dependency issue.

---

## Architecture

Layering follows DataTreeViewer, but the data model is tabular:

```text
Seer Plugin Host (QPluginLoader -> ViewerPluginInterface)
└── DTVPlugin                    plugin entry point, creates DataTableViewer
    └── DataTableViewer          ViewerBase subclass, thread/state management
        ├── TableWorker          background QThread, I/O + parsing
        │   └── ITableParser     parser interface in core/
        ├── TableRenderer        QTableView + TableModel + TableFilterProxy
        ├── TablePicker          SQLite table-list view
        ├── SearchBar            filter input
        └── StatusBar            rows, columns, format, warnings, filter status
```

Dependency rules:
- `core/` is STL-only. No Qt, no UI, no parser headers.
- `parsers/csv_parser.*` is STL + `core/` only.
- `parsers/sqlite_parser.*` may use Qt Core/Sql, but no Qt GUI/Widgets and no UI headers.
- `workers/` may know `ITableParser`, `TableData`, and Qt Core. No UI headers.
- `ui/table_renderer.*`, `table_model.*`, and `table_filter_proxy.*` know `TableData`, not parser implementations.
- `ui/data_table_viewer.*` is the integration layer and may include `core/`, `workers/`, `ui/`, and `seer/viewerbase.h`.

---

## Directory Structure

```text
DataTableViewer/
├── CMakeLists.txt
├── AGENTS.md
├── DataTableViewer-SPEC.md
├── bin/
│   └── plugin.json
├── src/
│   ├── core/
│   │   ├── table_data.h
│   │   ├── itable_parser.h
│   │   ├── type_inferrer.h/.cpp
│   │   └── parser_registry.h/.cpp
│   ├── parsers/
│   │   ├── csv_parser.h/.cpp
│   │   └── sqlite_parser.h/.cpp
│   ├── workers/
│   │   ├── background_thread.h
│   │   └── table_worker.h/.cpp
│   ├── ui/
│   │   ├── data_table_viewer.h/.cpp
│   │   ├── table_renderer.h/.cpp
│   │   ├── table_model.h/.cpp
│   │   ├── table_filter_proxy.h/.cpp
│   │   ├── table_picker.h/.cpp
│   │   ├── search_bar.h/.cpp
│   │   ├── status_bar.h/.cpp
│   │   └── style_assets.h
│   └── test.cpp
└── tests/
    ├── test_csv_parser.cpp
    ├── test_sqlite_parser.cpp
    ├── test_type_inferrer.cpp
    └── fixtures/
```

---

## Coding Conventions

| Category | Convention | Example |
|---|---|---|
| Classes / Structs | `PascalCase` | `TableData`, `TableRenderer` |
| Member variables | `m_` + `camelCase` | `m_generation`, `m_loadedRows` |
| Constants | `k` + `PascalCase` | `kSoftRowLimit`, `kFetchBatch` |
| Enums | `enum class` with `PascalCase` values | `ParseInputKind::Bytes` |
| Files | `snake_case` | `table_model.cpp` |
| Directories | `snake_case` | `src/core/` |
| Macros | `ALL_CAPS` | `REGISTER_TABLE_PARSER` |

Formatting:
- 4-space indentation.
- Opening brace on the same line for functions and control structures.
- `public:` / `private:` indented one level inside class bodies.
- Keep comments short and focused on non-obvious behavior.

Debug logging:
- UI/worker translation units may define `qprintt` near the top:
  ```cpp
  #define qprintt qDebug() << "[ClassName]"
  ```
- Do not add `qprintt` to `core/`.
- Avoid Qt dependencies in CSV/core code just for logging.

---

## Seer Plugin Metadata

`bin/plugin.json` is a release artifact and must stay tracked. CMake copies it
next to `datatableviewer.dll`:

```cmake
add_custom_command(TARGET datatableviewer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_SOURCE_DIR}/bin/plugin.json"
        "$<TARGET_FILE_DIR:datatableviewer>/plugin.json")
```

The manifest schema should match DataTreeViewer:

```json
{
  "name": "DataTableViewer",
  "version": "1.0.0",
  "type": "dll",
  "roles": ["viewer"],
  "entry": "datatableviewer.dll",
  "formats": ["csv", "tsv", "sqlite", "sqlite3", "db", "db3", "sl3"],
  "appMinVersion": "4.2.9",
  "author": "corey",
  "description": "Seer plugin for tabular data preview (CSV, TSV, SQLite)"
}
```

When adding or removing parser extensions, update all of these together:
1. `REGISTER_TABLE_PARSER(...)` registrations
2. `bin/plugin.json` `formats`
3. `src/test.cpp` file dialog filters, if relevant
4. parser unit tests
5. this AGENTS.md if the public support matrix changes

Do not ship a format in `plugin.json` unless `ParserRegistry::createParser()`
can create a parser for that same extension.

---

## Parser Contract

`ITableParser` is defined in `src/core/itable_parser.h`.

Input kinds:
- `ParseInputKind::Bytes`: worker reads the file, applies the 64 MB guard, strips UTF-8 BOM, and passes `ParseInput::bytes`.
- `ParseInputKind::Path`: worker does not read the file and passes `ParseInput::file_path`; SQLite uses this.

Result states:

| `ok` | `table_names` | `data` | Meaning |
|---|---|---|---|
| `true` | empty | non-null | table data ready |
| `true` | non-empty | null | SQLite table picker ready |
| `false` | empty | null | parse/open error |

Any other combination is a bug.

Parser lifetime rule:
- Parsers must copy all strings into owned `std::string` / `TableData` storage before `parse()` returns.
- Never store `std::string_view` from `ParseInput` beyond `parse()`.

Worker-owned metadata:
- Parsers set only `ok`, `error`, `err_line`, `warning`, `data`, and `table_names`.
- `TableWorker` fills `format_name`, `library_credit`, `file_bytes`, and `elapsed_ms`.
- `TableWorker` runs `TypeInferrer::infer()` only for byte-based parsers with table data.

---

## Format Rules

### CSV / TSV

`CsvParser` covers both CSV and TSV.

Expected behavior:
- CSV auto-detects comma vs tab from a small sample.
- TSV factory passes `'\t'` explicitly.
- First row is the header.
- Empty header cells become `Col0`, `Col1`, etc.
- Maximum columns are capped at `kMaxColumns`.
- Maximum parsed rows are capped at `kMaxParserRows`.
- Empty file is an error.
- UTF-8 BOM is stripped by `TableWorker`.

CSV parser must remain STL-only. Do not add Qt includes to `csv_parser.*`.

### SQLite

`SqliteParser` is two-phase:

1. Empty `ParseInput::table_name`: enumerate user tables and return `table_names`.
2. Non-empty `table_name`: query that table and return `TableData`.

Rules:
- SQLite is the only parser that may use `Qt6::Sql`.
- Use a unique connection name per parse.
- Destroy all `QSqlDatabase` / `QSqlQuery` objects before `QSqlDatabase::removeDatabase(name)`.
- Escape table names by wrapping in double quotes and doubling embedded double quotes.
- Reject or safely fail invalid paths and invalid table names.
- Fetch one extra row to detect truncation; do not run `SELECT COUNT(*)` in v1.

SQLite table picker mode is a successful viewer state. It must notify Seer with
`sigCommand(VCT_StateChange, VCV_Loaded)` after the picker is ready.

---

## ViewerBase Lifecycle

`DataTableViewer` inherits `ViewerBase`. The host calls `load(ctrl_bar, options)`.

Required overrides:
- `QString name() const` -> `"DataTableViewer"`
- `QSize getContentSize() const` -> `{960, 600}`
- `void loadImpl(QBoxLayout*, QHBoxLayout*)`
- `void updateDPR(qreal)`
- `void updateTheme(int)`

Lifecycle rules:
- `init()` is lazy and idempotent.
- `options()` is accessed only during the synchronous `loadImpl()` call. Do not store the pointer.
- `lay_ctrlbar` may be null. Guard it before adding buttons.
- `updateDPR()` and `updateTheme()` may be called before widgets exist. Guard null widgets or call `init()` intentionally.
- Search is disabled during loading and re-enabled only on successful parse/table-picker states.

State signaling:
- `VCV_Loading`: set by `ViewerBase::load()` before `loadImpl()`.
- `VCV_Loaded`: emit when a CSV/TSV table is rendered, when a SQLite table is rendered, and when the SQLite table picker is ready.
- `VCV_Error`: emit on unsupported extension, file open failure, parser failure, or SQLite open/query failure.

---

## Threading & Cancellation

Follow the DataTreeViewer pattern exactly:

```text
cancelPending()
  -> emit cancelRequested()
  -> ++m_generation

doLoadFile(path, table)
  -> create BackgroundThread
  -> create TableWorker
  -> worker->moveToThread(thread)
  -> thread started -> worker.doParse()
  -> worker parseCompleted(result, table, generation)
  -> discard if generation is stale
```

Rules:
- Never call `QThread::wait()` from `cancelPending()`.
- Worker owns parsing and self-deletes with `QScopeGuard -> deleteLater()`.
- Worker `destroyed` triggers `thread->deleteLater()`.
- `BackgroundThread::~BackgroundThread()` calls `quit(); wait();`.
- Stale results are discarded by generation checks in the UI thread.
- Check `QThread::isInterruptionRequested()` at meaningful worker boundaries.

Queued result type:

```cpp
qRegisterMetaType<std::shared_ptr<const dtv::core::TableParseResult>>();
```

Call this once from `DataTableViewer::init()`.

---

## UI Expectations

The first screen is the usable preview, not an explanatory page.

Table view:
- Read-only `QTableView`.
- Fixed default column width, movable/resizable columns.
- Numeric columns right-aligned and sort by numeric value.
- String columns sort locale-aware.
- Copy selected cells as TSV via `Ctrl+C`.
- `Ctrl+F` focuses the search field.

Search:
- Filtering is case-insensitive.
- Filter status should show match count or no-hit feedback.
- Filtering must not mutate underlying `TableData`.

SQLite picker:
- Shows table names after phase-1 parse.
- Uses a filter box when there are many tables.
- Selecting a table starts a new `doLoadFile(path, tableName)`.
- The back button returns from a loaded SQLite table to the picker.

Status bar:
- Shows format, row count, column count, load time, file size, library credit, warnings, and truncation hints.
- Truncated data must clearly indicate local/partial sort semantics if applicable.

Theme/DPI:
- Match DataTreeViewer's dark/light behavior.
- Avoid one-off hardcoded colors when a shared token exists in `style_assets.h`.
- Keep row heights, search bar, status bar, and picker text scaled by DPR.

---

## Adding A New Table Format

1. Add `src/parsers/xxx_parser.h/.cpp`.
2. Implement `ITableParser`.
3. Choose `ParseInputKind::Bytes` or `ParseInputKind::Path`.
4. Register extensions with `REGISTER_TABLE_PARSER(ext, factory)`.
5. Add files to `PARSER_SOURCES` in `CMakeLists.txt`.
6. Add every supported extension to `bin/plugin.json` `formats`.
7. Add parser tests and fixtures.
8. If the format returns multiple logical tables, reuse the `table_names` / `TablePicker` contract.

Do not change `TableWorker`, `TableRenderer`, or `DataTableViewer` for ordinary
single-table formats.

---

## Tests & Verification

Expected test coverage:
- CSV parser: basic CSV, quoted fields, embedded delimiter, TSV, fixtures, broken-but-graceful inputs.
- SQLite parser: missing file, table enumeration, table fetch, table-name escaping, empty tables, truncation.
- Type inference: int, float, bool, string, empty cells, mixed cells, numeric cache.
- UI/model tests should be added when changing `TableModel` or `TableFilterProxy` behavior.

Before claiming release readiness, run:

```powershell
cmake --preset RelWithDebInfo
cmake --build C:/Users/corey/Dev/build_output/DataTableViewer --target run_all_tests
cmake --build C:/Users/corey/Dev/build_output/DataTableViewer --target datatableviewer
```

Manual smoke checks:
- CSV loads and search/copy/sort work.
- TSV loads with tab delimiter.
- SQLite opens to picker, picker emits loaded state, table selection loads data, back returns to picker.
- Unsupported extension emits error state.
- Light/dark themes and DPR changes do not crash.

---

## v1 Non-Goals

- No editing or write-back.
- No XLSX, Parquet, Arrow, or other binary table formats.
- No SQL query editor.
- No joins or cross-table views.
- No memory-mapped I/O.
- No ANSI/CP1252 transcoding beyond graceful UTF-8 handling.
- No server-side SQLite sorting for truncated tables.

