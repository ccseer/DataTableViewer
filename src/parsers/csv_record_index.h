#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dtv::parsers {

#pragma pack(push, 1)
struct CsvIndexHeader {
    char magic[8] = {'D', 'T', 'V', 'C', 'S', 'V', '0', '1'};
    uint32_t headerSize = sizeof(CsvIndexHeader); // 64
    uint32_t version = 1;
    uint64_t headerStart = 0;       // Source file byte offset of header row
    uint64_t headerEnd = 0;         // Source file byte offset where header ends
    uint64_t dataRecordCount = 0;   // Number of indexed data records
    uint32_t columnCount = 0;       // Inferred column count
    char delimiter = ',';           // Detected delimiter (',' or '\t')
    uint8_t hasBom = 0;             // 1 if UTF-8 BOM present
    uint8_t isComplete = 0;         // 1 if EOF reached, 0 if still indexing
    uint8_t reserved[17] = {0};     // 47 bytes used, 17 reserved = 64 bytes total
};

struct CsvRecordSpan {
    uint64_t start = 0; // Source byte offset of record start (inclusive)
    uint64_t end = 0;   // Source byte offset of record end (exclusive of record delimiter)
};
#pragma pack(pop)

static_assert(sizeof(CsvIndexHeader) == 64, "CsvIndexHeader must be exactly 64 bytes");
static_assert(sizeof(CsvRecordSpan) == 16, "CsvRecordSpan must be exactly 16 bytes");

class CsvRecordIndex {
public:
    static constexpr size_t kMaxCachedSpans = 65536; // 1 MiB / 16 bytes = 65,536 spans

    CsvRecordIndex();
    ~CsvRecordIndex();

    CsvRecordIndex(const CsvRecordIndex &) = delete;
    CsvRecordIndex &operator=(const CsvRecordIndex &) = delete;

    // Not noexcept: the moved-from object is rebuilt in place, and that
    // allocation would terminate instead of propagating on failure.
    CsvRecordIndex(CsvRecordIndex &&);
    CsvRecordIndex &operator=(CsvRecordIndex &&);

    // Initialize temporary index file and write default header
    bool init();

    // Append a record span to the index
    bool appendSpan(uint64_t start, uint64_t end);

    // Read a single record span by 0-based data ordinal
    std::optional<CsvRecordSpan> readSpan(int64_t ordinal);

    // Read multiple record spans starting at firstOrdinal
    std::vector<CsvRecordSpan> readSpans(int64_t firstOrdinal, size_t count);

    // Header metadata accessors
    void setHeaderByteSpan(uint64_t start, uint64_t end);
    std::pair<uint64_t, uint64_t> headerByteSpan() const;

    void setMetadata(uint32_t colCount, char delimiter, bool hasBom);

    // Sets the EOF flag and flushes the write buffer plus the header to
    // disk. Returns false when the flush failed. A failed flush leaves the
    // index incomplete, so callers never observe EOF from a dead index.
    bool markComplete();

    // Full path of the temporary index file (UTF-8), for diagnostics and
    // tests. Empty when the index was never initialized.
    std::string filePath() const;

    // True only when EOF was reached *and* every write since then succeeded.
    // A failed flush must never advertise the index as final, because a
    // complete-looking index turns every page fetch into "no more rows".
    bool isComplete() const;
    // True after any write, seek or read failure. Every span query fails
    // from this point on, so no partially persisted range is served.
    bool hasFailed() const;
    uint64_t dataRecordCount() const { return m_header.dataRecordCount; }
    char delimiter() const { return m_header.delimiter; }
    bool hasBom() const { return m_header.hasBom != 0; }
    uint32_t columnCount() const { return m_header.columnCount; }

    bool flush();
    const std::string &error() const { return m_error; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    CsvIndexHeader m_header;
    std::string m_error;
};

} // namespace dtv::parsers
