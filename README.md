# DataTableViewer

**A tabular data viewer for Windows** - built as a native plugin for
[Seer](https://1218.io), the quick-look file preview tool.

DataTableViewer lets you preview CSV, TSV, and SQLite files in a fast read-only
table view. Press Space on a supported file in Seer to inspect rows, search
values, sort columns, and copy selected cells without opening a spreadsheet or
database tool.

## Features

- **CSV and TSV preview**: header detection, quoted-field handling, BOM stripping, and tab/comma support
- **SQLite preview**: browse database tables, pick one, then preview rows
- **Interactive table view**: sortable columns, movable/resizable headers, alternating rows, and TSV copy
- **Type-aware sorting**: numeric columns sort by numeric value instead of plain text
- **Live filtering**: search across the current table while keeping the UI responsive
- **Status bar metrics**: format, row count, column count, file size, load time, warnings, and truncation hints
- **Async parsing**: background-thread parsing and cancellation keep Seer responsive while switching files

## Supported Formats

- `.csv`
- `.tsv`
- `.sqlite`
- `.sqlite3`
- `.db`
- `.db3`
- `.sl3`

## Building

Requirements:

- Qt 6.8
- CMake 3.16+
- MSVC (Visual Studio 2022 or newer)
- A shell with the MSVC developer environment loaded

This project is intended to use a single `RelWithDebInfo` build:

```powershell
cmake --preset RelWithDebInfo
cmake --build C:/Users/corey/Dev/build_output/DataTableViewer --target datatableviewer
```

This produces:

- `datatableviewer.dll` - the Seer plugin
- `plugin.json` - copied next to the DLL after build

To run tests:

```powershell
cmake --build C:/Users/corey/Dev/build_output/DataTableViewer --target run_all_tests
```

Parser tests can be disabled with:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTS=OFF
```

## Use With Seer

[Seer](https://1218.io) is a quick-look file preview tool for Windows: press
Space on a file to preview it without opening a full application.

1. Install [Seer](https://1218.io).
2. In Seer, open **Settings -> Plugins**.
3. Install or place the DataTableViewer plugin files.
4. Ensure `datatableviewer.dll` and `plugin.json` are in the same plugin folder.
5. Press Space on a supported CSV, TSV, or SQLite file.

## Development Notes

Operational project rules live in [AGENTS.md](AGENTS.md). Use it for architecture,
format registration, lifecycle, threading, and release-check expectations.

## License

MIT © 2026 ccseer

