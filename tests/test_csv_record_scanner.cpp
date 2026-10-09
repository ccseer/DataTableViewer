#include <QtTest>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "parsers/csv_record_scanner.h"

struct DecodedRecord {
    uint64_t ordinal{0};
    uint64_t startOffset{0};
    uint64_t endOffset{0};
    std::vector<std::string> fields;
};

class TestCsvRecordScanner : public QObject {
    Q_OBJECT

private:
    std::vector<DecodedRecord> scanAll(std::string_view input, char delimiter, size_t chunkSize = 0)
    {
        std::vector<DecodedRecord> records;
        DecodedRecord current;
        std::string currentCell;

        dtv::parsers::CsvRecordScanner::Callbacks cb;
        cb.onFieldFragment = [&](size_t colIndex, std::string_view fragment, bool isEnd) {
            currentCell.append(fragment.data(), fragment.size());
            if(isEnd) {
                if(colIndex >= current.fields.size()) {
                    current.fields.resize(colIndex + 1);
                }
                current.fields[colIndex] = std::move(currentCell);
                currentCell.clear();
            }
        };

        cb.onRecord = [&](uint64_t ordinal, uint64_t start, uint64_t end) {
            current.ordinal = ordinal;
            current.startOffset = start;
            current.endOffset = end;
            records.push_back(std::move(current));
            current = DecodedRecord{};
            currentCell.clear();
        };

        dtv::parsers::CsvRecordScanner scanner(delimiter, std::move(cb));

        if(chunkSize == 0 || chunkSize >= input.size()) {
            scanner.feed(input, 0, true);
        } else {
            uint64_t offset = 0;
            while(offset < input.size()) {
                size_t len = std::min(chunkSize, input.size() - offset);
                bool isEof = (offset + len >= input.size());
                scanner.feed(input.substr(offset, len), offset, isEof);
                offset += len;
            }
        }

        return records;
    }

private slots:
    void testBasicCsv()
    {
        std::string input = "id,name,age\n1,Alice,30\n2,Bob,25\n";
        auto records = scanAll(input, ',');

        QCOMPARE(records.size(), 3ull);

        // Header
        QCOMPARE(records[0].ordinal, 0ull);
        QCOMPARE(records[0].startOffset, 0ull);
        QCOMPARE(records[0].endOffset, 11ull);
        QCOMPARE(
            input.substr(records[0].startOffset, records[0].endOffset - records[0].startOffset),
            std::string("id,name,age"));
        QCOMPARE(records[0].fields.size(), 3ull);
        QCOMPARE(records[0].fields[0], std::string("id"));
        QCOMPARE(records[0].fields[1], std::string("name"));
        QCOMPARE(records[0].fields[2], std::string("age"));

        // Row 1
        QCOMPARE(records[1].ordinal, 1ull);
        QCOMPARE(records[1].startOffset, 12ull);
        QCOMPARE(records[1].endOffset, 22ull);
        QCOMPARE(
            input.substr(records[1].startOffset, records[1].endOffset - records[1].startOffset),
            std::string("1,Alice,30"));
        QCOMPARE(records[1].fields[1], std::string("Alice"));
        QCOMPARE(records[1].fields[2], std::string("30"));

        // Row 2
        QCOMPARE(records[2].ordinal, 2ull);
        QCOMPARE(records[2].startOffset, 23ull);
        QCOMPARE(records[2].endOffset, 31ull);
        QCOMPARE(records[2].fields[1], std::string("Bob"));
    }

    void testCrlfAndCr()
    {
        std::string input = "a,b\r\n1,2\r3,4\n";
        auto records = scanAll(input, ',');

        QCOMPARE(records.size(), 3ull);

        // Record 0 (CRLF)
        QCOMPARE(records[0].startOffset, 0ull);
        QCOMPARE(records[0].endOffset, 3ull);
        QCOMPARE(
            input.substr(records[0].startOffset, records[0].endOffset - records[0].startOffset),
            std::string("a,b"));

        // Record 1 (CR)
        QCOMPARE(records[1].startOffset, 5ull);
        QCOMPARE(records[1].endOffset, 8ull);
        QCOMPARE(
            input.substr(records[1].startOffset, records[1].endOffset - records[1].startOffset),
            std::string("1,2"));

        // Record 2 (LF)
        QCOMPARE(records[2].startOffset, 9ull);
        QCOMPARE(records[2].endOffset, 12ull);
        QCOMPARE(
            input.substr(records[2].startOffset, records[2].endOffset - records[2].startOffset),
            std::string("3,4"));
    }

    void testQuotedCsvWithDoubledQuotes()
    {
        std::string input = "id,desc\n1,\"Hello, World\"\n2,\"He said \"\"Hi\"\" to me\"\n";
        auto records = scanAll(input, ',');

        QCOMPARE(records.size(), 3ull);
        QCOMPARE(records[1].fields[1], std::string("Hello, World"));
        QCOMPARE(records[2].fields[1], std::string("He said \"Hi\" to me"));
    }

    void testQuotedMultiline()
    {
        std::string input = "id,note\n1,\"Line 1\r\nLine 2\nLine 3\"\n2,Done\n";
        auto records = scanAll(input, ',');

        QCOMPARE(records.size(), 3ull);
        QCOMPARE(records[1].fields[0], std::string("1"));
        QCOMPARE(records[1].fields[1], std::string("Line 1\r\nLine 2\nLine 3"));
        QCOMPARE(records[2].fields[0], std::string("2"));
        QCOMPARE(records[2].fields[1], std::string("Done"));
    }

    void testUtf8Bom()
    {
        std::string input = "\xEF\xBB\xBFid,val\n1,100\n";
        auto records = scanAll(input, ',');

        QCOMPARE(records.size(), 2ull);
        // Header record start offset must skip BOM (offset 3)
        QCOMPARE(records[0].startOffset, 3ull);
        QCOMPARE(records[0].endOffset, 9ull);
        QCOMPARE(records[0].fields[0], std::string("id"));
        QCOMPARE(records[0].fields[1], std::string("val"));
    }

    void testByteByByteChunkFeeds()
    {
        // Feeds across every 1-byte split point: BOM, CRLF, escaped quotes, CJK UTF-8
        std::string input = "\xEF\xBB\xBF"
                            "序号,姓名,备注\r\n"
                            "1,\"张三\",\"你好\r\n\"\"世界\"\"\"\r\n"
                            "2,\"李四\",\"普通文本\"\n";

        auto expected = scanAll(input, ',', 0);

        // Test chunk sizes: 1, 2, 3, 5, 7, 11
        const std::vector<size_t> chunkSizes = {1, 2, 3, 5, 7, 11};
        for(size_t cs : chunkSizes) {
            auto actual = scanAll(input, ',', cs);
            QCOMPARE(actual.size(), expected.size());
            for(size_t i = 0; i < expected.size(); ++i) {
                QCOMPARE(actual[i].ordinal, expected[i].ordinal);
                QCOMPARE(actual[i].startOffset, expected[i].startOffset);
                QCOMPARE(actual[i].endOffset, expected[i].endOffset);
                QCOMPARE(actual[i].fields.size(), expected[i].fields.size());
                for(size_t j = 0; j < expected[i].fields.size(); ++j) {
                    QCOMPARE(actual[i].fields[j], expected[i].fields[j]);
                }
            }
        }
    }

    void testGiantFieldAcrossChunks()
    {
        // Giant quoted field > 300 KiB crossing 64 KiB and 256 KiB
        std::string giantText(300 * 1024, 'X');
        std::string input = "id,data\n1,\"" + giantText + "\"\n2,end\n";

        auto records = scanAll(input, ',', 64 * 1024);
        QCOMPARE(records.size(), 3ull);
        QCOMPARE(records[1].fields[1].size(), giantText.size());
        QCOMPARE(records[1].fields[1], giantText);
        QCOMPARE(records[2].fields[1], std::string("end"));
    }

    void testGiantUnquotedFieldAcrossChunks()
    {
        // Giant unquoted field > 300 KiB crossing 64 KiB chunks
        std::string giantText(300 * 1024, 'Y');
        std::string input = "id,data\n1," + giantText + "\n2,end\n";

        auto records = scanAll(input, ',', 64 * 1024);
        QCOMPARE(records.size(), 3ull);
        QCOMPARE(records[1].fields[1].size(), giantText.size());
        QCOMPARE(records[1].fields[1], giantText);
        QCOMPARE(records[2].fields[1], std::string("end"));
    }

    void testCancellationInsideQuotedField()
    {
        // Cancellation inside a giant quoted field
        std::string giantText(200 * 1024, 'Z');
        std::string input = "id,data\n1,\"" + giantText + "\"\n";

        dtv::parsers::CsvRecordScanner scanner(',', {});
        int checkCount = 0;
        bool ok = scanner.feed(input, 0, true, [&]() {
            checkCount++;
            return checkCount >= 2;
        });

        QVERIFY(!ok);
        QVERIFY(checkCount >= 2);
    }

    void testMalformedQuotesAtEof()
    {
        std::string input = "id,text\n1,\"This is fine\"\n2,\"This is broken\n3,Still broken?";
        auto records = scanAll(input, ',');

        QCOMPARE(records.size(), 3ull);
        QCOMPARE(records[0].fields[0], std::string("id"));
        QCOMPARE(records[1].fields[1], std::string("This is fine"));
        // Row 2 consumes remaining input due to unclosed quote
        QCOMPARE(records[2].fields[0], std::string("2"));
        QCOMPARE(records[2].fields[1], std::string("This is broken\n3,Still broken?"));
    }

    void testBlankLinesOptionA()
    {
        // Option A: blank line is treated as 1 empty field
        std::string input = "a,b\n\n1,2\n";
        auto records = scanAll(input, ',');

        QCOMPARE(records.size(), 3ull);
        QCOMPARE(records[0].fields.size(), 2ull);
        QCOMPARE(records[0].fields[0], std::string("a"));

        // Record 1 is empty line: 1 empty field
        QCOMPARE(records[1].fields.size(), 1ull);
        QCOMPARE(records[1].fields[0], std::string(""));

        // Record 2 is 1,2
        QCOMPARE(records[2].fields.size(), 2ull);
        QCOMPARE(records[2].fields[0], std::string("1"));
        QCOMPARE(records[2].fields[1], std::string("2"));
    }

    void testNoTrailingNewline()
    {
        std::string input = "col1,col2\nval1,val2";
        auto records = scanAll(input, ',');

        QCOMPARE(records.size(), 2ull);
        QCOMPARE(records[1].fields[0], std::string("val1"));
        QCOMPARE(records[1].fields[1], std::string("val2"));
        QCOMPARE(records[1].endOffset, static_cast<uint64_t>(input.size()));
    }

    void testHeaderOnly()
    {
        std::string input = "col1,col2\n";
        auto records = scanAll(input, ',');

        QCOMPARE(records.size(), 1ull);
        QCOMPARE(records[0].fields[0], std::string("col1"));
        QCOMPARE(records[0].fields[1], std::string("col2"));
    }

    void testTsvDelimiter()
    {
        std::string input = "col1\tcol2\nval1\tval2\n";
        auto records = scanAll(input, '\t');

        QCOMPARE(records.size(), 2ull);
        QCOMPARE(records[0].fields[0], std::string("col1"));
        QCOMPARE(records[0].fields[1], std::string("col2"));
        QCOMPARE(records[1].fields[0], std::string("val1"));
        QCOMPARE(records[1].fields[1], std::string("val2"));
    }

    void testAutoDetectDelimiter()
    {
        std::string csvSample = "a,b,c\n1,2,3\n";
        QCOMPARE(dtv::parsers::CsvRecordScanner::detectDelimiter(csvSample), ',');

        std::string tsvSample = "a\tb\tc\n1\t2\t3\n";
        QCOMPARE(dtv::parsers::CsvRecordScanner::detectDelimiter(tsvSample), '\t');
    }

    void testCancellation()
    {
        std::string input(200 * 1024, 'a');
        input += "\n";

        dtv::parsers::CsvRecordScanner scanner(',', {});
        int callCount = 0;
        bool result = scanner.feed(input, 0, true, [&]() {
            callCount++;
            return callCount >= 2; // Cancel after 2 checks
        });

        QVERIFY(!result);
        QVERIFY(callCount >= 2);
    }

    void testNonZeroBaseOffsetDoesNotProbeBom()
    {
        // When seeking into a file or decoding an indexed slice at baseOffset > 0,
        // bytes matching \xEF\xBB\xBF must not be stripped as BOM, and offsets must not be corrupted.
        std::string input = "\xEF\xBB\xBFsome_data\n";
        uint64_t seekOffset = 1000;

        std::vector<DecodedRecord> records;
        DecodedRecord current;
        std::string currentCell;

        dtv::parsers::CsvRecordScanner::Callbacks cb;
        cb.onFieldFragment = [&](size_t colIndex, std::string_view fragment, bool isEnd) {
            currentCell.append(fragment.data(), fragment.size());
            if(isEnd) {
                if(colIndex >= current.fields.size())
                    current.fields.resize(colIndex + 1);
                current.fields[colIndex] = std::move(currentCell);
                currentCell.clear();
            }
        };
        cb.onRecord = [&](uint64_t ordinal, uint64_t start, uint64_t end) {
            current.ordinal = ordinal;
            current.startOffset = start;
            current.endOffset = end;
            records.push_back(std::move(current));
            current = DecodedRecord{};
        };

        dtv::parsers::CsvRecordScanner scanner(',', std::move(cb));
        scanner.feed(input, seekOffset, true);

        QVERIFY(!scanner.hasBom());
        QCOMPARE(records.size(), 1ull);
        // Start offset must be seekOffset (1000), not corrupted to 0 or 3
        QCOMPARE(records[0].startOffset, seekOffset);
        QCOMPARE(records[0].endOffset, seekOffset + input.size() - 1); // minus \n
        // The \xEF\xBB\xBF bytes must be preserved in the field value
        QCOMPARE(records[0].fields[0], input.substr(0, input.size() - 1));
    }

    void testEmptyMidStreamChunkDoesNotDropPendingCr()
    {
        // Trace A: feed("abc\r", 0, false) + feed("", 4, false) + feed("\nd", 4, true)
        // "abc\r" has 4 bytes (>= 3 bytes), so BOM probing completes in chunk 1,
        // leaving \r as a truly pending CR across chunk boundaries.
        // The empty mid-stream feed must NOT clear m_pendingCr.
        // Chunk 3 with \n must resolve CRLF as a single record delimiter, producing 2 records [abc] and [d].
        std::vector<DecodedRecord> records;
        DecodedRecord current;
        std::string currentCell;

        dtv::parsers::CsvRecordScanner::Callbacks cb;
        cb.onFieldFragment = [&](size_t colIndex, std::string_view fragment, bool isEnd) {
            currentCell.append(fragment.data(), fragment.size());
            if(isEnd) {
                if(colIndex >= current.fields.size())
                    current.fields.resize(colIndex + 1);
                current.fields[colIndex] = std::move(currentCell);
                currentCell.clear();
            }
        };
        cb.onRecord = [&](uint64_t ordinal, uint64_t start, uint64_t end) {
            current.ordinal = ordinal;
            current.startOffset = start;
            current.endOffset = end;
            records.push_back(std::move(current));
            current = DecodedRecord{};
        };

        dtv::parsers::CsvRecordScanner scanner(',', std::move(cb));
        scanner.feed("abc\r", 0, false);
        scanner.feed("", 4, false);
        scanner.feed("\nd", 4, true);

        QCOMPARE(records.size(), 2ull);
        QCOMPARE(records[0].fields[0], std::string("abc"));
        QCOMPARE(records[1].fields[0], std::string("d"));
    }

    void testEmptyMidStreamChunkDoesNotDropPendingQuote()
    {
        // Trace B: feed("x,\"ab\"", 0, false) + feed("", 6, false) + feed("\"\",more", 6, true)
        // Must preserve pendingQuote across empty mid-stream feed so that the first quote in chunk 3
        // pairs with the trailing quote of chunk 1 into an escaped quote (""), and the second quote closes the field.
        std::vector<DecodedRecord> records;
        DecodedRecord current;
        std::string currentCell;

        dtv::parsers::CsvRecordScanner::Callbacks cb;
        cb.onFieldFragment = [&](size_t colIndex, std::string_view fragment, bool isEnd) {
            currentCell.append(fragment.data(), fragment.size());
            if(isEnd) {
                if(colIndex >= current.fields.size())
                    current.fields.resize(colIndex + 1);
                current.fields[colIndex] = std::move(currentCell);
                currentCell.clear();
            }
        };
        cb.onRecord = [&](uint64_t ordinal, uint64_t start, uint64_t end) {
            current.ordinal = ordinal;
            current.startOffset = start;
            current.endOffset = end;
            records.push_back(std::move(current));
            current = DecodedRecord{};
        };

        dtv::parsers::CsvRecordScanner scanner(',', std::move(cb));
        scanner.feed("x,\"ab\"", 0, false);
        scanner.feed("", 6, false);
        scanner.feed("\"\",more", 6, true);

        QCOMPARE(records.size(), 1ull);
        QCOMPARE(records[0].fields.size(), 3ull);
        QCOMPARE(records[0].fields[0], std::string("x"));
        QCOMPARE(records[0].fields[1], std::string("ab\""));
        QCOMPARE(records[0].fields[2], std::string("more"));
    }
};

QTEST_GUILESS_MAIN(TestCsvRecordScanner)
#include "test_csv_record_scanner.moc"
