# DataTableViewer Specification

## Parser Contract

CSV and TSV parsers materialize a `TableData` result. `SqliteParser` is used
for table discovery only: an empty table name returns the user table names.
Selected SQLite tables are loaded by `SqliteTableSource`.

## SQLite Source Lifetime

`SqliteTableSource` owns a read-only SQLite connection and remains alive for a
selected table. `SourceWorker` confines all source operations to its worker
thread and shuts the source down before the thread exits. The UI receives
immutable page snapshots and never owns SQLite handles.

## Threading and Cancellation

`viewGen` invalidates work for a different file or table. `opGen` invalidates
navigation and sorting requests within the selected table. Copy refetch uses an
independent `copyRequestId`; only the newest copy request may write to the
clipboard. Stale worker results are discarded by the UI generation checks.

## Sorting Capability

Server-side sorting is available only for SQLite objects with a usable rowid.
Paged SQLite sorting uses native SQLite ordering and deterministic rowid ties.
Tables without a supported row identity remain pageable but do not expose
server-side sorting.

## UI Behavior

Large SQLite tables use page navigation with a configurable page size and an
asynchronous row count. Filtering applies to the current page in this release.
Clamped cells are refetched asynchronously before copy; CSV and TSV remain
fully materialized and retain client-side sorting and copy behavior.

## Known Limitations

- Paged SQLite filtering is current-page-only.
- Natural-order restoration for paged SQLite is provided by re-entering the
  table; the paging header cycles between ascending and descending order.
- Markdown copy is limited to 1000 selected rows.
