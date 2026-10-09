#include "csv_record_scanner.h"

#include <algorithm>
#include <cstring>

namespace dtv::parsers {

namespace {
constexpr uint64_t kCancelCheckInterval = 65536; // 64 KiB
}

CsvRecordScanner::CsvRecordScanner(char delimiter, Callbacks cb)
    : m_delimiter(delimiter == '\0' ? ',' : delimiter), m_cb(std::move(cb))
{
}

char CsvRecordScanner::detectDelimiter(std::string_view bytes)
{
    std::string_view sample = bytes.substr(0, std::min<size_t>(bytes.size(), 4096));

    // Strip UTF-8 BOM if present at the start of sample
    if (sample.size() >= 3 && static_cast<unsigned char>(sample[0]) == 0xEF &&
        static_cast<unsigned char>(sample[1]) == 0xBB &&
        static_cast<unsigned char>(sample[2]) == 0xBF) {
        sample.remove_prefix(3);
    }

    int commaCount = 0;
    int tabCount = 0;
    int lines = 0;

    size_t pos = 0;
    while (pos < sample.size() && lines < 10) {
        size_t nextLine = sample.find('\n', pos);
        if (nextLine == std::string_view::npos)
            nextLine = sample.size();

        std::string_view line = sample.substr(pos, nextLine - pos);
        if (!line.empty()) {
            // Only delimiters outside quoted fields separate columns. Counting
            // inside quotes lets a single-column file whose values contain the
            // other candidate be detected as the wrong format.
            bool inQuotes = false;
            for (size_t i = 0; i < line.size(); ++i) {
                const char c = line[i];
                if (inQuotes) {
                    if (c == '"') {
                        inQuotes = (i + 1 < line.size() && line[i + 1] == '"');
                        if (inQuotes)
                            ++i;
                    }
                    continue;
                }
                if (c == '"') {
                    inQuotes = true;
                } else if (c == ',') {
                    commaCount++;
                } else if (c == '\t') {
                    tabCount++;
                }
            }
            lines++;
        }
        pos = (nextLine < sample.size()) ? nextLine + 1 : sample.size();
    }

    if (tabCount > commaCount * 2 && tabCount > 0)
        return '\t';
    return ',';
}

void CsvRecordScanner::reset()
{
    m_inQuotes = false;
    m_pendingQuote = false;
    m_pendingCr = false;
    m_hasBom = false;
    m_bomChecked = false;
    m_bomBuffer.clear();
    m_recordOrdinal = 0;
    m_currentRecordStart = 0;
    m_hasRecordStart = false;
    m_sawDataInRecord = false;
    m_colIndex = 0;
    m_bytesSinceCancelCheck = 0;
}

void CsvRecordScanner::emitFragment(std::string_view fragment, bool isEnd)
{
    if (m_cb.onFieldFragment && m_colIndex < kMaxColumns) {
        if (!fragment.empty() || isEnd) {
            m_cb.onFieldFragment(m_colIndex, fragment, isEnd);
        }
    }
}

void CsvRecordScanner::flushField()
{
    if (m_cb.onFieldFragment && m_colIndex < kMaxColumns) {
        m_cb.onFieldFragment(m_colIndex, "", true);
    }
}

void CsvRecordScanner::endRecord(uint64_t endOffset)
{
    if (m_cb.onRecord) {
        m_cb.onRecord(m_recordOrdinal, m_currentRecordStart, endOffset);
    }
    m_recordOrdinal++;
    m_colIndex = 0;
    m_sawDataInRecord = false;
    m_hasRecordStart = false;
}

bool CsvRecordScanner::feed(std::string_view chunk, uint64_t baseOffset, bool isEof,
                            const CancelCheck &cancel)
{
    const char *p = chunk.data();
    const char *end = p + chunk.size();

    // 1. Handle BOM probing
    if (!m_bomChecked) {
        if (baseOffset > 0 && m_bomBuffer.empty()) {
            // Non-zero offset on initial call: slice/seek decoding, skip BOM probe
            m_bomChecked = true;
            m_hasBom = false;
            m_currentRecordStart = baseOffset;
            m_hasRecordStart = true;
        } else if (m_bomBuffer.empty() && baseOffset == 0 && chunk.size() >= 3) {
            m_bomChecked = true;
            if (static_cast<unsigned char>(chunk[0]) == 0xEF &&
                static_cast<unsigned char>(chunk[1]) == 0xBB &&
                static_cast<unsigned char>(chunk[2]) == 0xBF) {
                m_hasBom = true;
                p += 3;
                m_currentRecordStart = 3;
                m_hasRecordStart = true;
            } else {
                m_hasBom = false;
                m_currentRecordStart = 0;
                m_hasRecordStart = true;
            }
        } else {
            size_t needed = 3 - m_bomBuffer.size();
            size_t take = std::min<size_t>(needed, chunk.size());
            m_bomBuffer.append(p, take);
            p += take;

            if (m_bomBuffer.size() >= 3 || isEof) {
                m_bomChecked = true;
                if (m_bomBuffer.size() >= 3 &&
                    static_cast<unsigned char>(m_bomBuffer[0]) == 0xEF &&
                    static_cast<unsigned char>(m_bomBuffer[1]) == 0xBB &&
                    static_cast<unsigned char>(m_bomBuffer[2]) == 0xBF) {
                    m_hasBom = true;
                    m_currentRecordStart = 3;
                    m_hasRecordStart = true;
                    m_bomBuffer.clear();
                } else {
                    m_hasBom = false;
                    m_currentRecordStart = 0;
                    m_hasRecordStart = true;
                    std::string buffered = std::move(m_bomBuffer);
                    m_bomBuffer.clear();
                    if (!buffered.empty()) {
                        if (!feed(buffered, 0, false, cancel))
                            return false;
                    }
                }
            } else {
                return true; // Still buffering the first 3 bytes
            }
        }
    }

    if (!m_hasRecordStart && m_recordOrdinal == 0) {
        m_currentRecordStart = baseOffset + (p - chunk.data());
        m_hasRecordStart = true;
    }

    // 2. Resolve pending CR from previous chunk
    if (m_pendingCr) {
        if (p < end) {
            m_pendingCr = false;
            if (*p == '\n') {
                p++; // consume LF of CRLF
            }
            m_currentRecordStart = baseOffset + (p - chunk.data());
            m_hasRecordStart = true;
        } else if (isEof) {
            m_pendingCr = false;
        }
    }

    // 3. Resolve pending quote from previous chunk
    if (m_pendingQuote) {
        if (p < end) {
            m_pendingQuote = false;
            if (*p == '"') {
                // Escaped quote across chunks: "" -> "
                emitFragment("\"", false);
                p++;
                m_inQuotes = true;
            } else {
                // Closing quote
                m_inQuotes = false;
            }
        } else if (isEof) {
            m_pendingQuote = false;
            m_inQuotes = false;
        }
    }

    const char *spanStart = p;

    while (p < end) {
        // Periodic cancellation check
        m_bytesSinceCancelCheck++;
        if (m_bytesSinceCancelCheck >= kCancelCheckInterval) {
            m_bytesSinceCancelCheck = 0;
            if (cancel && cancel()) {
                return false;
            }
        }

        char c = *p;

        if (m_inQuotes) {
            if (c == '"') {
                if (p > spanStart) {
                    emitFragment(std::string_view(spanStart, p - spanStart), false);
                }
                if (p + 1 < end && *(p + 1) == '"') {
                    // Escaped quote within chunk: "" -> "
                    emitFragment("\"", false);
                    p += 2;
                    spanStart = p;
                    continue;
                } else if (p + 1 == end && !isEof) {
                    // Trailing quote at end of chunk: wait for next chunk
                    m_pendingQuote = true;
                    p++;
                    spanStart = p;
                    break;
                } else {
                    // Closing quote
                    m_inQuotes = false;
                    p++;
                    spanStart = p;
                    continue;
                }
            } else {
                p++;
                continue;
            }
        } else {
            // Outside quotes
            if (!m_hasRecordStart) {
                m_currentRecordStart = baseOffset + (p - chunk.data());
                m_hasRecordStart = true;
            }

            if (c == '"') {
                if (p > spanStart) {
                    emitFragment(std::string_view(spanStart, p - spanStart), false);
                }
                m_sawDataInRecord = true;
                m_inQuotes = true;
                p++;
                spanStart = p;
                continue;
            } else if (c == m_delimiter) {
                if (p > spanStart) {
                    emitFragment(std::string_view(spanStart, p - spanStart), true);
                } else {
                    flushField();
                }
                m_sawDataInRecord = true;
                m_colIndex++;
                p++;
                spanStart = p;
                continue;
            } else if (c == '\r' || c == '\n') {
                if (p > spanStart) {
                    emitFragment(std::string_view(spanStart, p - spanStart), true);
                } else {
                    flushField();
                }

                uint64_t recordEndOffset = baseOffset + (p - chunk.data());

                if (c == '\r') {
                    if (p + 1 < end && *(p + 1) == '\n') {
                        p += 2;
                        endRecord(recordEndOffset);
                        m_currentRecordStart = baseOffset + (p - chunk.data());
                        m_hasRecordStart = true;
                        spanStart = p;
                        continue;
                    } else if (p + 1 == end && !isEof) {
                        m_pendingCr = true;
                        p++;
                        endRecord(recordEndOffset);
                        spanStart = p;
                        break;
                    } else {
                        // Standalone \r
                        p++;
                        endRecord(recordEndOffset);
                        m_currentRecordStart = baseOffset + (p - chunk.data());
                        m_hasRecordStart = true;
                        spanStart = p;
                        continue;
                    }
                } else {
                    // \n
                    p++;
                    endRecord(recordEndOffset);
                    m_currentRecordStart = baseOffset + (p - chunk.data());
                    m_hasRecordStart = true;
                    spanStart = p;
                    continue;
                }
            } else {
                m_sawDataInRecord = true;
                p++;
                continue;
            }
        }
    }

    if (p > spanStart) {
        emitFragment(std::string_view(spanStart, p - spanStart), false);
    }

    // 4. End of file handling
    if (isEof) {
        if (m_sawDataInRecord || m_inQuotes) {
            flushField();
            uint64_t recordEndOffset = baseOffset + chunk.size();
            endRecord(recordEndOffset);
        }
    }

    return true;
}

} // namespace dtv::parsers
