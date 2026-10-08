#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace dtv::parsers {

class CsvRecordScanner {
public:
    using CancelCheck = std::function<bool()>;

    static constexpr size_t kMaxColumns = 256;

    struct Callbacks {
        // Called when a complete logical record boundary is identified.
        // recordOrdinal: 0-based record index (0 is first record, e.g. header).
        // startOffset: inclusive source byte offset of record start.
        // endOffset: exclusive source byte offset of record end (excluding trailing \r/\n).
        std::function<void(uint64_t recordOrdinal, uint64_t startOffset, uint64_t endOffset)> onRecord;

        // Called for each field in the current record (if field decoding is requested).
        // colIndex: 0-based column index in the current record.
        // decodedFragment: unescaped/decoded byte fragment for this field.
        // isEnd: true when this field is complete.
        std::function<void(size_t colIndex, std::string_view decodedFragment, bool isEnd)> onFieldFragment;
    };

    explicit CsvRecordScanner(char delimiter = ',', Callbacks cb = {});

    // Feed a byte chunk into the scanner.
    // chunk: raw input bytes.
    // baseOffset: global source byte offset corresponding to the start of this chunk.
    // isEof: true if this is the final chunk of the stream.
    // cancel: optional cancellation check called periodically (at least every 64 KiB).
    // Returns true on success, false on cancellation or unrecoverable error.
    bool feed(std::string_view chunk, uint64_t baseOffset, bool isEof, const CancelCheck &cancel = {});

    void reset();

    bool inQuotes() const { return m_inQuotes; }
    char delimiter() const { return m_delimiter; }
    void setDelimiter(char d) { m_delimiter = d; }
    bool hasBom() const { return m_hasBom; }
    uint64_t recordCount() const { return m_recordOrdinal; }

    static char detectDelimiter(std::string_view sample);

private:
    void flushField();
    void emitFragment(std::string_view fragment, bool isEnd);
    void endRecord(uint64_t endOffset);

    char m_delimiter{','};
    Callbacks m_cb;

    // Parser state
    bool m_inQuotes{false};
    bool m_pendingQuote{false};     // Saw a quote inside quotes at chunk end or waiting for doubled quote
    bool m_pendingCr{false};        // Saw \r; waiting to check if followed by \n
    bool m_hasBom{false};
    bool m_bomChecked{false};
    std::string m_bomBuffer;

    uint64_t m_recordOrdinal{0};
    uint64_t m_currentRecordStart{0};
    bool m_hasRecordStart{false};
    bool m_sawDataInRecord{false};  // True if any data or delimiter or quote has been seen in current record

    size_t m_colIndex{0};

    uint64_t m_bytesSinceCancelCheck{0};
};

} // namespace dtv::parsers
