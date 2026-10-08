#include "csv_file_source.h"
#include "core/type_inferrer.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstdio>
#include <sys/stat.h>
#endif

namespace dtv::parsers {

namespace {
constexpr size_t kMaxSampleRows = 200;
constexpr size_t kMaxSampleCellBytes = 64;
constexpr size_t kMaxHeaderNameBytes = 4096;
constexpr size_t kMaxCopyBudget = 64 * 1024 * 1024; // 64 MiB
// A zero-byte slice budget never advances the scan, so a worker looping while
// the index is incomplete would spin forever. Floor the slice instead.
constexpr size_t kMinSliceBytes = 4096;
// Single owner of the external-mutation message: both the latching check and
// the identity re-check report it, so no caller has to match two spellings.
const char *kSourceChangedError = "Source file changed while it was being read";

size_t utf8SafeLength(std::string_view s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s.size();
    size_t len = maxBytes;
    while (len > 0 && (static_cast<unsigned char>(s[len]) & 0xC0) == 0x80) {
        len--;
    }
    return len;
}

std::vector<int> resolveTargetColumns(const std::vector<int> &requested, size_t colCount,
                                      std::vector<bool> &outSelected)
{
    const bool selectAll = requested.empty();
    outSelected.assign(colCount, selectAll);

    std::vector<int> targetCols;
    targetCols.reserve(colCount);
    if (selectAll) {
        for (size_t col = 0; col < colCount; ++col) {
            targetCols.push_back(static_cast<int>(col));
        }
    } else {
        for (int c : requested) {
            if (c >= 0 && static_cast<size_t>(c) < colCount && !outSelected[static_cast<size_t>(c)]) {
                outSelected[static_cast<size_t>(c)] = true;
                targetCols.push_back(c);
            }
        }
    }
    return targetCols;
}
} // namespace

struct CsvFileSource::Impl {
#ifdef _WIN32
    HANDLE fileHandle{INVALID_HANDLE_VALUE};
    uint64_t fileIndex{0};
    uint64_t lastWriteTime{0};
#else
    FILE *fileHandle{nullptr};
    dev_t fileIndex{0};
    ino_t fileSerial{0};
    time_t lastWriteTime{0};
#endif
    uint64_t fileSize{0};
    bool sourceChanged{false};
    std::string filePath;
    char delimiter{','};

    CsvRecordIndex index;
    std::vector<core::ColumnMeta> columns;
    std::optional<int64_t> knownTotal;
    core::CancelCheck cancelCheck;

    // Background indexing state
    uint64_t scanOffset{0};
    std::unique_ptr<CsvRecordScanner> indexScanner;
    bool hasScannedHeader{false};
    bool scanFailed{false};

    // Sample-scan state owned here because the stored scanner's callbacks reference it.
    bool sampling{false};
    std::vector<std::string> sampleRow;
    std::string sampleCell;
    core::TableData sampleData;
    std::vector<bool> longSampleCells;
    // Set per column while sampling whenever a cell was cut short by the
    // sample cap. The stored cell alone cannot carry that fact, so a column
    // whose values were trimmed never infers as numeric.
    std::vector<bool> trimmedSampleCells;

    // Everything a reopen has to drop before any of it is read again. Keeping
    // this list in one method is what separates a clean reopen from a source
    // that silently indexes with the previous file's size.
    void resetForOpen()
    {
        filePath.clear();
        fileSize = 0;
        sourceChanged = false;
        scanOffset = 0;
        hasScannedHeader = false;
        scanFailed = false;
        sampling = false;
        sampleRow.clear();
        sampleCell.clear();
        sampleData = core::TableData{};
        longSampleCells.clear();
        trimmedSampleCells.assign(CsvRecordScanner::kMaxColumns, false);
        columns.clear();
        knownTotal.reset();
        indexScanner.reset();
    }

    ~Impl()
    {
        close();
    }

    void close()
    {
#ifdef _WIN32
        if (fileHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(fileHandle);
            fileHandle = INVALID_HANDLE_VALUE;
        }
#else
        if (fileHandle) {
            fclose(fileHandle);
            fileHandle = nullptr;
        }
#endif
    }

    bool readBytes(uint64_t offset, size_t size, std::string &outBytes, std::string &err)
    {
#ifdef _WIN32
        if (fileHandle == INVALID_HANDLE_VALUE) {
            err = "Source file is not open";
            return false;
        }
        if (size > 0xFFFFFFFFull) {
            err = "Read request exceeds the Win32 read limit";
            return false;
        }
        outBytes.resize(size);
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(fileHandle, li, nullptr, FILE_BEGIN)) {
            err = "Failed to seek source file";
            return false;
        }
        DWORD bytesRead = 0;
        if (!ReadFile(fileHandle, outBytes.data(), static_cast<DWORD>(size), &bytesRead, nullptr) ||
            bytesRead != static_cast<DWORD>(size)) {
            err = "Failed to read requested bytes from source file";
            return false;
        }
        outBytes.resize(bytesRead);
        return true;
#else
        if (!fileHandle) {
            err = "Source file is not open";
            return false;
        }
        outBytes.resize(size);
        if (fseek(fileHandle, static_cast<long>(offset), SEEK_SET) != 0) {
            err = "Failed to seek source file";
            return false;
        }
        size_t read = fread(outBytes.data(), 1, size, fileHandle);
        if (read != size) {
            err = "Failed to read requested bytes from source file";
            return false;
        }
        outBytes.resize(read);
        return true;
#endif
    }

    // The source file is opened shared, so another process may rewrite or
    // truncate it mid-preview. Identity plus size plus write time is the
    // strongest cheap check available; the documented blind spot remains an
    // in-place edit that restores both the size and the timestamp. Once a
    // change is seen it is latched: every recorded span may already point at
    // bytes that no longer hold the record they were measured against.
    bool verifyUnchanged(std::string &err)
    {
        if (sourceChanged) {
            err = kSourceChangedError;
            return false;
        }
#ifdef _WIN32
        if (fileHandle == INVALID_HANDLE_VALUE) {
            err = "Source file is not open";
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(fileHandle, &info)) {
            err = "Failed to stat source file";
            return false;
        }
        const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        const uint64_t index = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        const uint64_t writeTime = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                                   info.ftLastWriteTime.dwLowDateTime;
        if (index != fileIndex || size != fileSize || writeTime != lastWriteTime) {
            sourceChanged = true;
            err = kSourceChangedError;
            return false;
        }
#else
        if (!fileHandle) {
            err = "Source file is not open";
            return false;
        }
        struct stat info;
        if (fstat(fileno(fileHandle), &info) != 0) {
            err = "Failed to stat source file";
            return false;
        }
        if (info.st_dev != fileIndex || info.st_ino != fileSerial ||
            static_cast<uint64_t>(info.st_size) != fileSize ||
            static_cast<uint64_t>(info.st_mtime) != lastWriteTime) {
            sourceChanged = true;
            err = kSourceChangedError;
            return false;
        }
#endif
        return true;
    }
};

CsvFileSource::CsvFileSource(char delimiter)
    : m_impl(std::make_unique<Impl>()), m_initialDelimiter(delimiter)
{
    m_impl->delimiter = delimiter;
}

CsvFileSource::~CsvFileSource() = default;

std::string CsvFileSource::indexFilePath() const
{
    return m_impl->index.filePath();
}

const std::vector<core::ColumnMeta> &CsvFileSource::columns() const
{
    return m_impl->columns;
}

std::optional<int64_t> CsvFileSource::rowCount() const
{
    return m_impl->knownTotal;
}

void CsvFileSource::setKnownTotal(int64_t total)
{
    if (total >= 0) {
        m_impl->knownTotal = total;
    }
}

void CsvFileSource::setCancelCheck(core::CancelCheck cancel)
{
    m_impl->cancelCheck = std::move(cancel);
}

bool CsvFileSource::open(const std::string &path, char delimiter)
{
    m_impl->close();
    m_error.clear();
    m_impl->resetForOpen();
    m_impl->filePath = path;

    char targetDelim = (delimiter != '\0') ? delimiter : m_initialDelimiter;
    m_impl->delimiter = targetDelim;

    if (!m_impl->index.init()) {
        m_error = "Failed to initialize index: " + m_impl->index.error();
        return false;
    }

#ifdef _WIN32
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                      static_cast<int>(path.size()), nullptr, 0);
    if (wideLen <= 0) {
        m_error = "Invalid file path encoding";
        return false;
    }
    std::wstring wpath(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                        wpath.data(), wideLen);
    m_impl->fileHandle = CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (m_impl->fileHandle == INVALID_HANDLE_VALUE) {
        m_error = "Failed to open file: " + std::to_string(GetLastError());
        return false;
    }

    BY_HANDLE_FILE_INFORMATION fileInfo;
    if (GetFileInformationByHandle(m_impl->fileHandle, &fileInfo)) {
        m_impl->fileSize = (static_cast<uint64_t>(fileInfo.nFileSizeHigh) << 32) | fileInfo.nFileSizeLow;
        m_impl->fileIndex = (static_cast<uint64_t>(fileInfo.nFileIndexHigh) << 32) | fileInfo.nFileIndexLow;
        m_impl->lastWriteTime = (static_cast<uint64_t>(fileInfo.ftLastWriteTime.dwHighDateTime) << 32) |
                                fileInfo.ftLastWriteTime.dwLowDateTime;
    }
#else
    m_impl->fileHandle = fopen(path.c_str(), "rb");
    if (!m_impl->fileHandle) {
        m_error = "Failed to open file";
        return false;
    }
    struct stat statInfo;
    if (fstat(fileno(m_impl->fileHandle), &statInfo) == 0) {
        m_impl->fileSize = static_cast<uint64_t>(statInfo.st_size);
        m_impl->fileIndex = statInfo.st_dev;
        m_impl->fileSerial = statInfo.st_ino;
        m_impl->lastWriteTime = statInfo.st_mtime;
    }
#endif

    if (m_impl->fileSize == 0) {
        m_error = "Empty file";
        return false;
    }

    // 1. Delimiter resolution
    if (m_impl->delimiter == '\0') {
        std::string sample;
        std::string err;
        if (!m_impl->readBytes(0, std::min<size_t>(4096, m_impl->fileSize), sample, err)) {
            m_error = err;
            return false;
        }
        m_impl->delimiter = CsvRecordScanner::detectDelimiter(sample);
    }

    // 2. Initial sample scan (read up to 200 data rows or EOF)
    m_impl->sampling = true;
    m_impl->sampleData = core::TableData{};

    CsvRecordScanner::Callbacks cb;
    cb.onFieldFragment = [this](size_t colIndex, std::string_view fragment, bool isEnd) {
        if (!m_impl->sampling || colIndex >= CsvRecordScanner::kMaxColumns) {
            return;
        }
        // Sample cells of *data* rows are capped at 64 decoded bytes; whatever
        // does not fit still counts as "long" and pins the column to String.
        // Record 0 is the header, not sample data: it keeps every byte here and
        // is bounded separately by the header-name cap below.
        const size_t trimmed = m_impl->hasScannedHeader ? kMaxSampleCellBytes : kMaxHeaderNameBytes;
        const size_t stored = m_impl->sampleCell.size();
        const size_t room = stored < trimmed ? trimmed - stored : 0;
        const size_t take = utf8SafeLength(fragment, room);
        m_impl->sampleCell.append(fragment.data(), take);
        if (take < fragment.size()) {
            m_impl->trimmedSampleCells[colIndex] = true;
        }
        if (isEnd) {
            m_impl->sampleRow.push_back(std::move(m_impl->sampleCell));
            m_impl->sampleCell.clear();
        }
    };

    cb.onRecord = [this](uint64_t recordOrdinal, uint64_t start, uint64_t end) {
        if (recordOrdinal == 0) {
            // Header record: first logical record provides the column names.
            m_impl->index.setHeaderByteSpan(start, end);
            for (size_t i = 0; i < m_impl->sampleRow.size(); ++i) {
                core::ColumnMeta meta;
                meta.name = std::move(m_impl->sampleRow[i]);
                // Second bound on the same rule: the header name is part of the
                // frozen schema, so enforce the cap here too rather than rely
                // on the scan callback alone.
                if (meta.name.size() > kMaxHeaderNameBytes) {
                    meta.name.resize(utf8SafeLength(meta.name, kMaxHeaderNameBytes));
                }
                if (meta.name.empty())
                    meta.name = "Col" + std::to_string(i);
                m_impl->sampleData.columns.push_back(std::move(meta));
            }
            m_impl->longSampleCells.assign(m_impl->sampleData.columns.size(), false);
            m_impl->trimmedSampleCells.assign(CsvRecordScanner::kMaxColumns, false);
            m_impl->hasScannedHeader = true;
        } else {
            // Data record
            if (!m_impl->index.appendSpan(start, end)) {
                m_impl->scanFailed = true;
                m_error = "Failed to append index span: " + m_impl->index.error();
                return;
            }
            if (m_impl->sampling && m_impl->sampleData.rows.size() < kMaxSampleRows) {
                auto &row = m_impl->sampleRow;
                // TypeInferrer indexes every row by column position, so a short
                // row has to be padded here exactly like a displayed row is.
                if (row.size() < m_impl->longSampleCells.size()) {
                    row.resize(m_impl->longSampleCells.size());
                }
                for (size_t col = 0; col < row.size() && col < m_impl->longSampleCells.size(); ++col) {
                    if (row[col].size() > kMaxSampleCellBytes || m_impl->trimmedSampleCells[col]) {
                        m_impl->longSampleCells[col] = true;
                    }
                }
                m_impl->sampleData.rows.push_back(std::move(row));
            }
        }
        m_impl->sampleRow.clear();
        m_impl->sampleCell.clear();
        m_impl->trimmedSampleCells.assign(CsvRecordScanner::kMaxColumns, false);
    };

    auto scanner = std::make_unique<CsvRecordScanner>(m_impl->delimiter, std::move(cb));
    constexpr size_t kChunkSize = 256 * 1024;
    constexpr size_t kMaxSampleScanBytes = 2 * 1024 * 1024; // 2 MiB bound

    while (m_impl->scanOffset < m_impl->fileSize) {
        size_t toRead = std::min<size_t>(kChunkSize, m_impl->fileSize - m_impl->scanOffset);
        std::string chunk;
        std::string readErr;
        if (!m_impl->readBytes(m_impl->scanOffset, toRead, chunk, readErr)) {
            m_error = readErr;
            return false;
        }
        bool isEof = (m_impl->scanOffset + toRead >= m_impl->fileSize);
        if (!scanner->feed(chunk, m_impl->scanOffset, isEof, m_impl->cancelCheck)) {
            m_error = "Initial scan cancelled or failed";
            return false;
        }
        m_impl->scanOffset += toRead;

        if (m_impl->scanFailed) {
            return false;
        }

        if (m_impl->hasScannedHeader && (m_impl->sampleData.rows.size() >= kMaxSampleRows || isEof || m_impl->scanOffset >= kMaxSampleScanBytes)) {
            break;
        }
    }

    if (!m_impl->hasScannedHeader || m_impl->sampleData.columns.empty()) {
        m_error = "No data found";
        return false;
    }

    // 3. Type inference over sample, then freeze the schema
    core::TypeInferrer::infer(m_impl->sampleData);
    for (size_t col = 0; col < m_impl->sampleData.columns.size(); ++col) {
        if (m_impl->longSampleCells[col]) {
            m_impl->sampleData.columns[col].type = core::ColumnMeta::Type::String;
        }
    }
    m_impl->columns = std::move(m_impl->sampleData.columns);

    m_impl->sampling = false;
    m_impl->sampleData = core::TableData{};
    m_impl->sampleRow.clear();
    m_impl->sampleCell.clear();

    m_impl->index.setMetadata(
        static_cast<uint32_t>(m_impl->columns.size()),
        m_impl->delimiter,
        scanner->hasBom());

    bool isEof = (m_impl->scanOffset >= m_impl->fileSize);

    // Small-file fast path: if EOF was reached in initial chunks
    if (isEof) {
        if (!m_impl->index.markComplete()) {
            m_error = "Failed to finalize index: " + m_impl->index.error();
            return false;
        }
        m_impl->knownTotal = m_impl->index.dataRecordCount();
    } else {
        m_impl->indexScanner = std::move(scanner);
    }

    return true;
}

core::IndexReadiness CsvFileSource::readiness(int64_t firstOrdinal, int pageSize) const
{
    if (firstOrdinal < 0 || pageSize <= 0)
        return core::IndexReadiness::Failed;
    if (m_impl->sourceChanged || m_impl->index.hasFailed())
        return core::IndexReadiness::Failed;

    // Unsigned arithmetic keeps an extreme ordinal from overflowing into a
    // "covered" result.
    const uint64_t first = static_cast<uint64_t>(firstOrdinal);
    if (m_impl->index.dataRecordCount() >= first + static_cast<uint64_t>(pageSize))
        return core::IndexReadiness::Ready;

    if (m_impl->index.isComplete()) {
        if (first >= m_impl->index.dataRecordCount())
            return core::IndexReadiness::End;
        return core::IndexReadiness::Ready; // Partial last page
    }

    return core::IndexReadiness::Pending;
}

core::IndexProgress CsvFileSource::advanceIndex(size_t byteBudget, core::CancelCheck cancel)
{
    core::IndexProgress progress;
    if (m_impl->sourceChanged) {
        progress.error = kSourceChangedError;
        progress.indexedRows = m_impl->index.dataRecordCount();
        progress.scannedBytes = m_impl->scanOffset;
        return progress;
    }
    if (m_impl->index.hasFailed()) {
        progress.error = "Index failed: " + m_impl->index.error();
        progress.indexedRows = m_impl->index.dataRecordCount();
        progress.scannedBytes = m_impl->scanOffset;
        return progress;
    }
    if (m_impl->index.isComplete()) {
        progress.indexedRows = m_impl->index.dataRecordCount();
        progress.scannedBytes = m_impl->fileSize;
        progress.isComplete = true;
        return progress;
    }

    if (!m_impl->indexScanner) {
        progress.error = "Source is not open";
        return progress;
    }
    if (m_impl->scanFailed) {
        progress.error = "Indexing was cancelled or failed";
        progress.indexedRows = m_impl->index.dataRecordCount();
        progress.scannedBytes = m_impl->scanOffset;
        return progress;
    }

    std::string statsErr;
    if (!m_impl->verifyUnchanged(statsErr)) {
        progress.error = statsErr;
        progress.indexedRows = m_impl->index.dataRecordCount();
        progress.scannedBytes = m_impl->scanOffset;
        return progress;
    }

    const size_t budget = (byteBudget < kMinSliceBytes) ? kMinSliceBytes : byteBudget;
    size_t toRead = std::min<size_t>(budget, m_impl->fileSize - m_impl->scanOffset);
    std::string chunk;
    std::string err;
    if (!m_impl->readBytes(m_impl->scanOffset, toRead, chunk, err)) {
        progress.error = err;
        return progress;
    }

    bool isEof = (m_impl->scanOffset + toRead >= m_impl->fileSize);
    if (!m_impl->indexScanner->feed(chunk, m_impl->scanOffset, isEof, cancel)) {
        m_impl->scanFailed = true;
        progress.error = "Indexing cancelled";
        progress.indexedRows = m_impl->index.dataRecordCount();
        progress.scannedBytes = m_impl->scanOffset;
        return progress;
    }
    m_impl->scanOffset += toRead;

    if (m_impl->scanFailed) {
        progress.error = "Indexing failed: " + m_impl->index.error();
        progress.indexedRows = m_impl->index.dataRecordCount();
        progress.scannedBytes = m_impl->scanOffset;
        return progress;
    }

    if (isEof) {
        if (!m_impl->index.markComplete()) {
            progress.error = m_impl->index.error().empty() ? "Failed to finalize index"
                                                           : m_impl->index.error();
            progress.indexedRows = m_impl->index.dataRecordCount();
            progress.scannedBytes = m_impl->scanOffset;
            progress.isComplete = false;
            return progress;
        }
        m_impl->knownTotal = m_impl->index.dataRecordCount();
        progress.isComplete = true;
    }

    progress.indexedRows = m_impl->index.dataRecordCount();
    progress.scannedBytes = m_impl->scanOffset;
    return progress;
}

size_t CsvFileSource::computePerCellCap(int pageSize, size_t pageColumnCount) const
{
    if (pageSize <= 0 || pageColumnCount == 0)
        return 4096;
    size_t budgetPerCell = (32 * 1024 * 1024) / (static_cast<size_t>(pageSize) * pageColumnCount);
    return std::min<size_t>(4096, std::max<size_t>(16, budgetPerCell));
}

core::PageResult CsvFileSource::readPage(int64_t startOrdinal, int pageSize)
{
    core::PageResult result;
    if (startOrdinal < 0 || pageSize <= 0) {
        result.error = "Invalid page parameters";
        return result;
    }

    std::string statsErr;
    if (!m_impl->verifyUnchanged(statsErr)) {
        result.error = statsErr;
        return result;
    }

    if (m_impl->cancelCheck && m_impl->cancelCheck()) {
        result.error = "Cancelled";
        return result;
    }

    uint64_t totalIndexed = m_impl->index.dataRecordCount();
    if (static_cast<uint64_t>(startOrdinal) >= totalIndexed) {
        if (!m_impl->index.isComplete()) {
            // Indexing has not reached this range yet. Reporting success with
            // an empty page would present a temporary end as a final one.
            result.error = "Page range is not indexed yet";
            return result;
        }
        result.ok = true;
        auto emptyData = std::make_shared<core::TableData>();
        emptyData->columns = m_impl->columns;
        result.data = std::move(emptyData);
        result.token.offset = startOrdinal;
        result.token.valid = false;
        result.hasMore = false;
        return result;
    }

    size_t countToRead = std::min<size_t>(
        static_cast<size_t>(pageSize),
        static_cast<size_t>(totalIndexed > static_cast<uint64_t>(startOrdinal) ? totalIndexed - startOrdinal : 0));

    auto spans = m_impl->index.readSpans(startOrdinal, countToRead);
    if (spans.size() != countToRead) {
        result.error = "Failed to read index spans";
        return result;
    }

    auto data = std::make_shared<core::TableData>();
    data->columns = m_impl->columns;
    size_t colCount = m_impl->columns.size();
    size_t cellCap = computePerCellCap(pageSize, colCount);

    result.keys.reserve(spans.size());
    result.clamped.reserve(spans.size());
    data->rows.reserve(spans.size());

    // Resolve the cache vectors once: every entry of by_column is stable after
    // this loop, so the row loop below needs no further hash lookups.
    std::vector<std::vector<double> *> numericColumns(colCount, nullptr);
    for (size_t col = 0; col < colCount; ++col) {
        if (m_impl->columns[col].type == core::ColumnMeta::Type::Integer ||
            m_impl->columns[col].type == core::ColumnMeta::Type::Float) {
            auto &cache = data->numeric_cache.by_column[static_cast<int>(col)];
            cache.reserve(spans.size());
            numericColumns[col] = &cache;
        }
    }

    std::vector<std::string> rowValues;
    std::vector<bool> rowClamped;
    std::string cell;
    bool cellWasClamped = false;

    CsvRecordScanner::Callbacks cb;
    cb.onFieldFragment = [&](size_t colIndex, std::string_view fragment, bool isEnd) {
        if (colIndex < colCount) {
            if (cell.size() < cellCap) {
                size_t allowed = cellCap - cell.size();
                size_t safeLen = utf8SafeLength(fragment, allowed);
                cell.append(fragment.data(), safeLen);
                if (safeLen < fragment.size()) {
                    cellWasClamped = true;
                }
            } else if (!fragment.empty()) {
                cellWasClamped = true;
            }

            if (isEnd) {
                rowValues.push_back(std::move(cell));
                rowClamped.push_back(cellWasClamped);
                cell.clear();
                cellWasClamped = false;
            }
        }
    };

    CsvRecordScanner rowScanner(m_impl->delimiter, std::move(cb));

    std::string rawRecord; // Reused across rows so its capacity survives.
    for (size_t rowIdx = 0; rowIdx < spans.size(); ++rowIdx) {
        if (m_impl->cancelCheck && m_impl->cancelCheck()) {
            result.error = "Cancelled";
            return result;
        }

        const auto &span = spans[rowIdx];
        size_t recordBytes = static_cast<size_t>(span.end - span.start);
        std::string err;
        if (!m_impl->readBytes(span.start, recordBytes, rawRecord, err)) {
            result.error = err;
            return result;
        }

        rowValues.clear();
        rowClamped.clear();
        cell.clear();
        cellWasClamped = false;

        if (!rowScanner.feed(rawRecord, span.start, true, m_impl->cancelCheck)) {
            result.ok = false;
            result.error = "Cancelled";
            return result;
        }
        rowScanner.reset();

        // Pad short rows
        while (rowValues.size() < colCount) {
            rowValues.push_back("");
            rowClamped.push_back(false);
        }

        // Numeric cache population
        for (size_t col = 0; col < colCount; ++col) {
            std::vector<double> *cache = numericColumns[col];
            if (!cache)
                continue;
            double numeric = std::numeric_limits<double>::quiet_NaN();
            if (!rowClamped[col]) {
                std::string_view text = rowValues[col];
                if (!text.empty() && text.front() == '+') {
                    text.remove_prefix(1);
                }
                double parsed;
                const auto conv = std::from_chars(text.data(), text.data() + text.size(), parsed);
                if (conv.ec == std::errc{} && conv.ptr == text.data() + text.size()) {
                    numeric = parsed;
                }
            }
            cache->push_back(numeric);
        }

        core::RefetchKey key;
        key.csvOrdinal = startOrdinal + static_cast<int64_t>(rowIdx);
        result.keys.push_back(std::move(key));
        result.clamped.push_back(std::move(rowClamped));
        data->rows.push_back(std::move(rowValues));
    }

    result.token.offset = startOrdinal;
    result.token.valid = !data->rows.empty();
    if (!spans.empty()) {
        result.token.firstKey = spans.front().start;
        result.token.lastKey = spans.back().end;
    }

    result.hasMore = (m_impl->index.dataRecordCount() > static_cast<uint64_t>(startOrdinal + spans.size())) ||
                     !m_impl->index.isComplete();

    if (m_impl->knownTotal) {
        data->total_rows = static_cast<size_t>(*m_impl->knownTotal);
    }

    result.data = std::move(data);
    result.ok = true;
    return result;
}

namespace {
core::PageResult tokenFailure()
{
    core::PageResult result;
    result.error = "Invalid page token";
    return result;
}
} // namespace

core::PageResult CsvFileSource::first(int pageSize)
{
    return readPage(0, pageSize);
}

core::PageResult CsvFileSource::next(const core::PageToken &token, int pageSize)
{
    if (!token.valid)
        return tokenFailure();
    if (token.offset > (std::numeric_limits<int64_t>::max)() - pageSize)
        return tokenFailure();
    return readPage(token.offset + pageSize, pageSize);
}

core::PageResult CsvFileSource::prev(const core::PageToken &token, int pageSize)
{
    if (!token.valid)
        return tokenFailure();
    int64_t target = std::max<int64_t>(0, token.offset - pageSize);
    return readPage(target, pageSize);
}

core::PageResult CsvFileSource::last(int pageSize, std::optional<int64_t> knownTotal)
{
    if (!knownTotal.has_value() || *knownTotal < 0 || pageSize <= 0) {
        core::PageResult res;
        res.error = "Known total and valid page size required for last";
        return res;
    }
    if (*knownTotal == 0)
        return first(pageSize);

    int64_t total = *knownTotal;
    int64_t remainder = total % pageSize;
    int64_t target = remainder == 0 ? total - pageSize : total - remainder;
    target = std::max<int64_t>(0, target);
    return readPage(target, pageSize);
}

core::RefetchResult CsvFileSource::refetch(const core::RefetchKey &key)
{
    core::RefetchResult result;
    if (!key.csvOrdinal.has_value()) {
        result.error = "Missing CSV ordinal";
        return result;
    }

    std::string statsErr;
    if (!m_impl->verifyUnchanged(statsErr)) {
        result.error = statsErr;
        return result;
    }

    if (m_impl->cancelCheck && m_impl->cancelCheck()) {
        result.error = "Cancelled";
        return result;
    }

    auto span = m_impl->index.readSpan(*key.csvOrdinal);
    if (!span) {
        result.error = "Record ordinal out of range";
        return result;
    }

    size_t recordBytes = static_cast<size_t>(span->end - span->start);
    std::string rawRecord;
    std::string err;
    if (!m_impl->readBytes(span->start, recordBytes, rawRecord, err)) {
        result.error = err;
        return result;
    }

    const size_t colCount = m_impl->columns.size();
    std::vector<std::string> allValues(colCount);
    std::vector<bool> selected;
    std::vector<int> targetCols = resolveTargetColumns(key.selectedColumns, colCount, selected);

    size_t totalBytes = 0;
    bool budgetExceeded = false;

    CsvRecordScanner::Callbacks cb;
    cb.onFieldFragment = [&](size_t colIndex, std::string_view fragment, bool /*isEnd*/) {
        if (budgetExceeded)
            return;
        if (colIndex < colCount && selected[colIndex]) {
            totalBytes += fragment.size();
            if (totalBytes > kMaxCopyBudget) {
                budgetExceeded = true;
                return;
            }
            allValues[colIndex].append(fragment.data(), fragment.size());
        }
    };

    CsvRecordScanner scanner(m_impl->delimiter, std::move(cb));
    auto cancelRefetch = [&]() -> bool {
        return budgetExceeded || (m_impl->cancelCheck && m_impl->cancelCheck());
    };
    if (!scanner.feed(rawRecord, span->start, true, cancelRefetch)) {
        result.ok = false;
        result.error = budgetExceeded ? "Copy budget exceeded (64 MiB)" : "Cancelled";
        return result;
    }

    if (budgetExceeded) {
        result.ok = false;
        result.error = "Copy budget exceeded (64 MiB)";
        return result;
    }

    for (int col : targetCols) {
        result.columns.push_back(col);
        result.values.push_back(std::move(allValues[static_cast<size_t>(col)]));
    }

    result.ok = true;
    return result;
}

} // namespace dtv::parsers
