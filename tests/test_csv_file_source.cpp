#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryFile>
#include <string>
#include <vector>

#include "parsers/csv_file_source.h"
#include "parsers/csv_record_index.h"
#include "core/table_source.h"
#include "core/table_data.h"

class TestCsvFileSource : public QObject {
    Q_OBJECT

private:
    // Creates a temp file with the given content. An optional template name
    // (must contain XXXXXX) exercises non-ASCII path handling.
    QString createTempCsv(const std::string &content, const QString &templateName = QString())
    {
        auto *file = new QTemporaryFile(this);
        if (!templateName.isEmpty()) {
            file->setFileTemplate(QDir::tempPath() + "/" + templateName);
        }
        if (!file->open()) {
            return QString();
        }
        file->write(content.data(), static_cast<qint64>(content.size()));
        file->flush();
        QString path = file->fileName();
        file->close();
        return path;
    }

private slots:
    void testEmptyAndErrorCases()
    {
        // 1. Non-existent file
        dtv::parsers::CsvFileSource nonExistent;
        QVERIFY(!nonExistent.open("non_existent_file_xyz_123.csv"));
        QVERIFY(!nonExistent.error().empty());

        // 2. Empty file
        QString emptyPath = createTempCsv("");
        dtv::parsers::CsvFileSource emptySource;
        QVERIFY(!emptySource.open(emptyPath.toStdString()));
        QCOMPARE(emptySource.error(), std::string("Empty file"));
    }

    void testHeaderOnlyFile()
    {
        QString path = createTempCsv("col1,col2,col3\n");
        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        QCOMPARE(source.columns().size(), 3ull);
        QCOMPARE(source.columns()[0].name, std::string("col1"));
        QCOMPARE(source.columns()[1].name, std::string("col2"));
        QCOMPARE(source.columns()[2].name, std::string("col3"));

        // Row count is 0
        QVERIFY(source.rowCount().has_value());
        QCOMPARE(*source.rowCount(), 0LL);

        auto page = source.first(500);
        QVERIFY(page.ok);
        QVERIFY(page.data->rows.empty());
        QVERIFY(!page.hasMore);
    }

    void testPageNavigationFirstNextPrevLast()
    {
        // 1250 records with 3 columns: id,name,val
        std::string content = "id,name,val\n";
        content.reserve(1250 * 25);
        for (int i = 0; i < 1250; ++i) {
            content += std::to_string(i) + ",Name_" + std::to_string(i) + "," + std::to_string(i * 10) + "\n";
        }
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));
        QCOMPARE(source.columns().size(), 3ull);

        // Page 1: first 500 rows (0..499)
        auto p1 = source.first(500);
        QVERIFY(p1.ok);
        QCOMPARE(p1.data->rows.size(), 500ull);
        QCOMPARE(p1.data->rows[0][0], std::string("0"));
        QCOMPARE(p1.data->rows[499][0], std::string("499"));
        QCOMPARE(p1.token.offset, 0LL);
        QVERIFY(p1.token.valid);
        QVERIFY(p1.hasMore);

        // Page 2: next 500 rows (500..999)
        auto p2 = source.next(p1.token, 500);
        QVERIFY(p2.ok);
        QCOMPARE(p2.data->rows.size(), 500ull);
        QCOMPARE(p2.data->rows[0][0], std::string("500"));
        QCOMPARE(p2.data->rows[499][0], std::string("999"));
        QCOMPARE(p2.token.offset, 500LL);
        QVERIFY(p2.hasMore);

        // Page 3: remaining 250 rows (1000..1249)
        auto p3 = source.next(p2.token, 500);
        QVERIFY(p3.ok);
        QCOMPARE(p3.data->rows.size(), 250ull);
        QCOMPARE(p3.data->rows[0][0], std::string("1000"));
        QCOMPARE(p3.data->rows[249][0], std::string("1249"));
        QCOMPARE(p3.token.offset, 1000LL);
        QVERIFY(!p3.hasMore);

        // Prev from page 3 goes to page 2 (500..999)
        auto pPrev = source.prev(p3.token, 500);
        QVERIFY(pPrev.ok);
        QCOMPARE(pPrev.data->rows.size(), 500ull);
        QCOMPARE(pPrev.data->rows[0][0], std::string("500"));

        // Last page: 1250 total rows with pageSize 500 -> start ordinal 1000 (rows 1000..1249)
        auto pLast = source.last(500, 1250);
        QVERIFY(pLast.ok);
        QCOMPARE(pLast.data->rows.size(), 250ull);
        QCOMPARE(pLast.data->rows[0][0], std::string("1000"));
        QCOMPARE(pLast.data->rows[249][0], std::string("1249"));
        QVERIFY(!pLast.hasMore);
    }

    void testExactPageBoundary()
    {
        // 1000 rows with pageSize 500 (exact boundary)
        std::string content = "k,v\n";
        for (int i = 0; i < 1000; ++i) {
            content += std::to_string(i) + ",val\n";
        }
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        auto p1 = source.first(500);
        QVERIFY(p1.ok);
        QCOMPARE(p1.data->rows.size(), 500ull);
        QVERIFY(p1.hasMore);

        auto p2 = source.next(p1.token, 500);
        QVERIFY(p2.ok);
        QCOMPARE(p2.data->rows.size(), 500ull);
        QCOMPARE(p2.data->rows[499][0], std::string("999"));
        QVERIFY(!p2.hasMore);
    }

    void testDynamicClampingAndUtf8Boundary()
    {
        // Generate a row with a huge cell containing CJK multibyte characters
        // "测试数据" in UTF-8 is 12 bytes (4 characters x 3 bytes each).
        std::string cjkPattern = "测试数据";
        std::string longText;
        while (longText.size() < 10000) {
            longText += cjkPattern;
        }

        std::string content = "id,desc\n1,\"" + longText + "\"\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        auto page = source.first(500);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows.size(), 1ull);

        // Cell must be clamped
        QVERIFY(page.clamped[0][1]);
        const std::string &clampedStr = page.data->rows[0][1];
        QVERIFY(clampedStr.size() <= 4096);
        QVERIFY(clampedStr.size() > 0);

        // Verify valid UTF-8 by converting to QString and back
        QString qstr = QString::fromUtf8(clampedStr.data(), static_cast<int>(clampedStr.size()));
        QVERIFY(!qstr.contains(QChar::ReplacementCharacter));
    }

    void testTypeInferenceAndFreeze()
    {
        // Row 1: integer, float, string, and long number (> 64 bytes)
        std::string longNum(70, '9');
        std::string content = "int_col,float_col,str_col,long_num\n"
                              "123,45.67,hello," + longNum + "\n"
                              "456,89.12,world," + longNum + "\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        const auto &cols = source.columns();
        QCOMPARE(cols.size(), 4ull);
        QCOMPARE(cols[0].type, dtv::core::ColumnMeta::Type::Integer);
        QCOMPARE(cols[1].type, dtv::core::ColumnMeta::Type::Float);
        QCOMPARE(cols[2].type, dtv::core::ColumnMeta::Type::String);
        // Column 3 has cells > 64 bytes in sample, so it must be forced to String
        QCOMPARE(cols[3].type, dtv::core::ColumnMeta::Type::String);
    }

    void testRefetchSelectedColumnsAndFullValue()
    {
        std::string longText(8000, 'A');
        std::string content = "id,colA,colB\n1,\"" + longText + "\",small_val\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        auto page = source.first(500);
        QVERIFY(page.ok);
        QVERIFY(page.clamped[0][1]); // colA is clamped

        // 1. Refetch only column 1 (colA)
        dtv::core::RefetchKey key1;
        key1.csvOrdinal = 0;
        key1.selectedColumns = {1};

        auto res1 = source.refetch(key1);
        QVERIFY(res1.ok);
        QCOMPARE(res1.columns.size(), 1ull);
        QCOMPARE(res1.columns[0], 1);
        QCOMPARE(res1.values.size(), 1ull);
        QCOMPARE(res1.values[0], longText);

        // 1b. Duplicate column indices in selectedColumns must be deduplicated
        dtv::core::RefetchKey keyDup;
        keyDup.csvOrdinal = 0;
        keyDup.selectedColumns = {1, 1};

        auto resDup = source.refetch(keyDup);
        QVERIFY(resDup.ok);
        QCOMPARE(resDup.columns.size(), 1ull);
        QCOMPARE(resDup.columns[0], 1);
        QCOMPARE(resDup.values.size(), 1ull);
        QCOMPARE(resDup.values[0], longText);

        // 2. Refetch all columns (empty selectedColumns)
        dtv::core::RefetchKey keyAll;
        keyAll.csvOrdinal = 0;

        auto resAll = source.refetch(keyAll);
        QVERIFY(resAll.ok);
        QCOMPARE(resAll.columns.size(), 3ull);
        QCOMPARE(resAll.values[0], std::string("1"));
        QCOMPARE(resAll.values[1], longText);
        QCOMPARE(resAll.values[2], std::string("small_val"));
    }

    void testReadinessAndBackgroundIndexing()
    {
        // Generate a file > 300 KiB so it requires multiple 256 KiB slices
        std::string content = "id,data\n";
        content.reserve(500 * 1024);
        for (int i = 0; i < 15000; ++i) {
            content += std::to_string(i) + ",some_filler_text_for_padding\n";
        }
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        // Page 1 (rows 0..499) was scanned in initial chunk
        QCOMPARE(source.readiness(0, 500), dtv::core::IndexReadiness::Ready);

        // Page far beyond initial 256 KiB chunk returns Pending
        QCOMPARE(source.readiness(14000, 500), dtv::core::IndexReadiness::Pending);

        // Advance indexing until EOF with iteration bound
        int slices = 0;
        while (!source.advanceIndex(256 * 1024).isComplete) {
            QVERIFY(++slices < 100);
        }

        // Now row 14000 is Ready
        QCOMPARE(source.readiness(14000, 500), dtv::core::IndexReadiness::Ready);

        // Past total rows returns End
        QCOMPARE(source.readiness(20000, 500), dtv::core::IndexReadiness::End);

        // The chunk boundary must not have split or shifted any record:
        // walk every page and verify the id column is the exact 0..14999
        // sequence. A split record would corrupt the row at the boundary and
        // shift every ordinal after it.
        QCOMPARE(*source.rowCount(), 15000LL);
        int expected = 0;
        auto page = source.first(500);
        while (page.ok && !page.data->rows.empty()) {
            for (const auto &row : page.data->rows) {
                QCOMPARE(row[0], std::to_string(expected));
                expected++;
            }
            if (!page.hasMore) {
                break;
            }
            page = source.next(page.token, 500);
        }
        QVERIFY(page.ok);
        QCOMPARE(expected, 15000);
    }

    void testInvalidTokenRejected()
    {
        QString path = createTempCsv("id,data\n1,a\n2,b\n");
        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        // A default-constructed token is invalid and must hit the same
        // failure path as the SQLite source instead of silently navigating.
        dtv::core::PageToken invalid;
        QVERIFY(!invalid.valid);
        auto nextResult = source.next(invalid, 500);
        QVERIFY(!nextResult.ok);
        QCOMPARE(nextResult.error, std::string("Invalid page token"));
        auto prevResult = source.prev(invalid, 500);
        QVERIFY(!prevResult.ok);
        QCOMPARE(prevResult.error, std::string("Invalid page token"));
    }

    void testTempIndexFileCleanup()
    {
        QString path = createTempCsv("id,data\n1,a\n2,b\n");
        std::string indexPath;
        {
            dtv::parsers::CsvFileSource source;
            QVERIFY(source.open(path.toStdString()));
            indexPath = source.indexFilePath();
            QVERIFY(!indexPath.empty());
            // The backing index file exists while the source is open.
            QVERIFY(QFile::exists(QString::fromStdString(indexPath)));
        }
        // DELETE_ON_CLOSE must remove the index file when the source dies.
        QVERIFY(!QFile::exists(QString::fromStdString(indexPath)));
    }

    void testNonAsciiPath()
    {
        // A CJK file name proves the source opens UTF-8 paths correctly;
        // widening the bytes one by one would break every non-ASCII path.
        std::string content = "id,\xE6\xB5\x8B\xE8\xAF\x95\n1,\xE5\x80\xBC\n";
        QString path = createTempCsv(content, "dtv_\xE6\xB5\x8B\xE8\xAF\x95_XXXXXX.csv");
        QVERIFY(!path.isEmpty());

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));
        QVERIFY(source.error().empty());

        auto page = source.first(500);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows.size(), 1ull);
        QCOMPARE(page.data->rows[0][1], std::string("\xE5\x80\xBC"));
    }

    void testReopeningWithDifferentDelimiters()
    {
        // Prove that opening a TSV then a CSV on the same instance does not inherit the previous delimiter
        QString tsvPath = createTempCsv("colA\tcolB\n1\t2\n");
        QString csvPath = createTempCsv("x,y\n3,4\n");

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(tsvPath.toStdString(), '\0'));
        QCOMPARE(source.columns().size(), 2ull);
        QCOMPARE(source.columns()[0].name, std::string("colA"));
        QCOMPARE(source.columns()[1].name, std::string("colB"));

        // Reopening with a CSV file on the same instance must re-detect comma
        QVERIFY(source.open(csvPath.toStdString(), '\0'));
        QCOMPARE(source.columns().size(), 2ull);
        QCOMPARE(source.columns()[0].name, std::string("x"));
        QCOMPARE(source.columns()[1].name, std::string("y"));

        auto page = source.first(500);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows.size(), 1ull);
        QCOMPARE(page.data->rows[0][0], std::string("3"));
        QCOMPARE(page.data->rows[0][1], std::string("4"));
    }

    void testShortRowAndBlankLineDoNotBreakInference()
    {
        // Every sampled row has to be full width: TypeInferrer indexes rows by
        // column position, so an unpadded short row reads past the row vector.
        // A blank physical line becomes a one-empty-field record, which is the
        // shortest row a file can produce.
        QString path = createTempCsv("n,int_col,extra\n1,10,a\n2\n\n3,30,c\n");
        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));
        QCOMPARE(source.columns().size(), 3ull);
        QCOMPARE(source.columns()[0].type, dtv::core::ColumnMeta::Type::Integer);
        QCOMPARE(source.columns()[1].type, dtv::core::ColumnMeta::Type::Integer);

        // Four data records survive the sample: "1,10,a", the short row "2",
        // the blank line, and "3,30,c".
        QCOMPARE(*source.rowCount(), 4LL);
        auto page = source.first(500);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows.size(), 4ull);
        for (const auto &row : page.data->rows) {
            QCOMPARE(row.size(), 3ull);
        }
        QCOMPARE(page.data->rows[1][0], std::string("2"));
        QCOMPARE(page.data->rows[1][2], std::string(""));
        QCOMPARE(page.data->rows[2][0], std::string(""));
        QCOMPARE(page.data->rows[3][1], std::string("30"));
    }

    void testHeaderNamesAreCapped()
    {
        // A header value is bounded to 4096 decoded bytes and the cut has to
        // land on a UTF-8 code point boundary. 测试数据 is 4 x 3 bytes.
        std::string cjk;
        while (cjk.size() < 20000)
            cjk += "\xE6\xB5\x8B\xE8\xAF\x95";
        std::string content = "id,\"" + cjk + "\"\n1,value\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));
        QCOMPARE(source.columns().size(), 2ull);

        const std::string &name = source.columns()[1].name;
        QVERIFY(name.size() <= 4096);
        QVERIFY(name.size() > 0);
        QString decoded = QString::fromUtf8(name.data(), static_cast<int>(name.size()));
        QVERIFY(!decoded.contains(QChar::ReplacementCharacter));

        // The cap applies to the schema, not to the data under that column.
        auto page = source.first(500);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows[0][1], std::string("value"));
    }

    void testClampedCjkRefetchLabelsColumns()
    {
        // The refetch result carries its own column labels, so a multi-byte
        // clamped value has to come back under the column it belongs to.
        std::string cjk;
        while (cjk.size() < 10000)
            cjk += "\xE6\xB5\x8B\xE8\xAF\x95";
        std::string content = "id,first,wide\n1,start,\"" + cjk + "\"\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));
        auto page = source.first(500);
        QVERIFY(page.ok);
        QVERIFY(page.clamped[0][2]);

        dtv::core::RefetchKey key;
        key.csvOrdinal = 0;
        key.selectedColumns = {2};
        auto res = source.refetch(key);
        QVERIFY(res.ok);
        QCOMPARE(res.columns.size(), 1ull);
        QCOMPARE(res.columns[0], 2);
        QCOMPARE(res.values.size(), 1ull);
        QCOMPARE(res.values[0], cjk);
        QString decoded = QString::fromUtf8(res.values[0].data(), static_cast<int>(res.values[0].size()));
        QVERIFY(!decoded.contains(QChar::ReplacementCharacter));
    }

    void testFailedIndexFinalizeIsNeverComplete()
    {
        // Positive control: a finalized index reports EOF.
        dtv::parsers::CsvRecordIndex good;
        QVERIFY(good.init());
        QVERIFY(good.appendSpan(10, 20));
        QVERIFY(good.markComplete());
        QVERIFY(good.isComplete());
        QVERIFY(!good.hasFailed());
        auto span = good.readSpan(0);
        QVERIFY(span.has_value());
        QCOMPARE(span->start, 10ull);
        QCOMPARE(span->end, 20ull);

        // A finalize whose flush fails must roll the EOF flag back; otherwise
        // the source advertises N rows as final while every span read fails.
        dtv::parsers::CsvRecordIndex failed;
        QVERIFY(!failed.markComplete());
        QVERIFY(!failed.isComplete());
        QVERIFY(failed.hasFailed());
        QVERIFY(!failed.readSpan(0).has_value());
    }

    void testPageBeyondIndexedTailFails()
    {
        std::string content = "id,data\n";
        for (int i = 0; i < 15000; ++i)
            content += std::to_string(i) + ",some_filler_text_for_padding\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));
        QCOMPARE(source.readiness(14000, 500), dtv::core::IndexReadiness::Pending);

        // An unindexed range must not come back as a successful empty page:
        // that would present a temporary end of file as a final one.
        auto beyond = source.last(500, 20000);
        QVERIFY(!beyond.ok);
        QCOMPARE(beyond.error, std::string("Page range is not indexed yet"));

        int slices = 0;
        while (!source.advanceIndex(256 * 1024).isComplete)
            QVERIFY(++slices < 100);

        // Past the real last row the index is final, so the empty page is true.
        auto pastEnd = source.last(500, 20000);
        QVERIFY(pastEnd.ok);
        QVERIFY(pastEnd.data->rows.empty());
        QVERIFY(!pastEnd.hasMore);
    }

    void testExternalMutationStopsIndexingAndPaging()
    {
        std::string content = "id,data\n";
        for (int i = 0; i < 15000; ++i)
            content += std::to_string(i) + ",some_filler_text_for_padding\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));
        auto firstPage = source.first(500);
        QVERIFY(firstPage.ok);

        // Rewrite the file behind the source's back. The share mode allows it,
        // size and write time both change, and every stored span may now point
        // at bytes that no longer hold the record they were measured against.
        // Any write counts: size and write time both change.
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(file.write("\n999999,tampered\n"), static_cast<qint64>(17));
        file.close();

        auto mutated = source.first(500);
        QVERIFY(!mutated.ok);
        QCOMPARE(mutated.error, std::string("Source file changed while it was being read"));
        QCOMPARE(source.readiness(0, 500), dtv::core::IndexReadiness::Failed);

        // Indexing must not continue from stale offsets either.
        auto progress = source.advanceIndex(256 * 1024);
        QVERIFY(!progress.isComplete);
        QCOMPARE(progress.error, std::string("Source file changed while it was being read"));
    }

    void testHeaderExceedingInitialChunk()
    {
        // Header line > 300 KiB exceeding the initial 256 KiB read window
        std::string longCol(300 * 1024, 'H');
        std::string content = "id,\"" + longCol + "\"\n1,val\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));
        QCOMPARE(source.columns().size(), 2ull);
        QCOMPARE(source.columns()[0].name, std::string("id"));

        auto page = source.first(500);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows.size(), 1ull);
        QCOMPARE(page.data->rows[0][0], std::string("1"));
        QCOMPARE(page.data->rows[0][1], std::string("val"));
    }

    void testLeadingPlusInNumericColumns()
    {
        // Leading + signs in numbers must not evaluate to NaN in numeric_cache
        QString path = createTempCsv("id,val\n+123,+45.67\n");
        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        QCOMPARE(source.columns().size(), 2ull);
        QCOMPARE(source.columns()[0].type, dtv::core::ColumnMeta::Type::Integer);
        QCOMPARE(source.columns()[1].type, dtv::core::ColumnMeta::Type::Float);

        auto page = source.first(500);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows.size(), 1ull);
        QCOMPARE(page.data->rows[0][0], std::string("+123"));
        QCOMPARE(page.data->rows[0][1], std::string("+45.67"));

        // Verify numeric_cache contains valid parsed numbers, not NaN
        auto &numCache = page.data->numeric_cache.by_column;
        QVERIFY(numCache.find(0) != numCache.end());
        QVERIFY(numCache.find(1) != numCache.end());
        QCOMPARE(numCache.at(0)[0], 123.0);
        QCOMPARE(numCache.at(1)[0], 45.67);
    }

    void testHeaderOver4096DoesNotForceDataColumnsToString()
    {
        // Header exceeding 4096 bytes must be capped for header metadata,
        // but must NOT leak trimmed flag into data rows to force numeric columns to String.
        std::string longHeaderName(5000, 'A');
        std::string content = "id,\"" + longHeaderName + "\"\n1,100\n2,200\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        QCOMPARE(source.columns().size(), 2ull);
        QCOMPARE(source.columns()[0].type, dtv::core::ColumnMeta::Type::Integer);
        // Column 1 values are 100 and 200; it must infer as Integer, not String!
        QCOMPARE(source.columns()[1].type, dtv::core::ColumnMeta::Type::Integer);
    }

    void testRefetchFailsWhenSourceFileModified()
    {
        QString path = createTempCsv("id,val\n1,hello\n2,world\n");
        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        // Tamper with the source file by appending bytes
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Append));
        file.write("3,tampered\n");
        file.close();

        // Refetch must call verifyUnchanged and fail
        dtv::core::RefetchKey key;
        key.csvOrdinal = 0;
        auto res = source.refetch(key);
        QVERIFY(!res.ok);
        QCOMPARE(res.error, std::string("Source file changed while it was being read"));
    }

    void testAppendSpanInvertedOffsetsRejected()
    {
        dtv::parsers::CsvRecordIndex index;
        QVERIFY(index.init());
        // Inverted span (start > end) must be rejected defensively
        QVERIFY(!index.appendSpan(500, 100));
        QCOMPARE(index.dataRecordCount(), 0ull);
    }

    void testMoveConstructorAndAssignmentRemainSafe()
    {
        dtv::parsers::CsvRecordIndex idx1;
        QVERIFY(idx1.init());
        QVERIFY(idx1.appendSpan(10, 20));
        QCOMPARE(idx1.dataRecordCount(), 1ull);

        // Move construction
        dtv::parsers::CsvRecordIndex idx2(std::move(idx1));
        QCOMPARE(idx2.dataRecordCount(), 1ull);
        auto span = idx2.readSpan(0);
        QVERIFY(span.has_value());
        QCOMPARE(span->start, 10ull);
        QCOMPARE(span->end, 20ull);

        // Moved-from idx1 must be safe against all accessor calls
        QVERIFY(idx1.hasFailed());
        QVERIFY(!idx1.isComplete());
        QVERIFY(!idx1.appendSpan(30, 40));
        QVERIFY(!idx1.readSpan(0).has_value());
        QVERIFY(!idx1.markComplete());
        QVERIFY(!idx1.flush());
        QVERIFY(idx1.filePath().empty());

        // Move assignment
        dtv::parsers::CsvRecordIndex idx3;
        idx3 = std::move(idx2);
        QCOMPARE(idx3.dataRecordCount(), 1ull);
        auto span3 = idx3.readSpan(0);
        QVERIFY(span3.has_value());
        QCOMPARE(span3->start, 10ull);

        // Moved-from idx2 must be safe against all accessor calls
        QVERIFY(idx2.hasFailed());
        QVERIFY(!idx2.isComplete());
        QVERIFY(!idx2.readSpan(0).has_value());
    }

    void testAdvanceIndexIgnoresOperationCancelCheck()
    {
        // Background indexing must watch only the slice's cancel parameter,
        // never m_impl->cancelCheck (which carries opGen from navigation).
        // If it did, any page turn would kill background indexing.
        std::string content = "id,data\n";
        for (int i = 0; i < 15000; ++i) {
            content += std::to_string(i) + ",filler_text_padding\n";
        }
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));

        // Simulate a navigation operation setting opGen-bound cancelCheck
        source.setCancelCheck([]() { return true; });

        // advanceIndex should still make progress without being cancelled
        auto progress = source.advanceIndex(256 * 1024);
        QVERIFY(progress.error.empty());
        QVERIFY(progress.indexedRows > 0);
    }

    void testClampedNumericCellPutsNanInNumericCache()
    {
        // First 200 rows establish numeric type (integer).
        // Later row contains a huge clamped number (> 4096 bytes).
        // The truncated prefix must NOT be parsed into numeric_cache; it must store NaN.
        std::string content = "id,num\n";
        for (int i = 0; i < 200; ++i) {
            content += std::to_string(i) + "," + std::to_string(i * 10) + "\n";
        }
        std::string giantDigits(5000, '9');
        content += "200," + giantDigits + "\n";
        QString path = createTempCsv(content);

        dtv::parsers::CsvFileSource source;
        QVERIFY(source.open(path.toStdString()));
        QCOMPARE(source.columns().size(), 2ull);
        QCOMPARE(source.columns()[1].type, dtv::core::ColumnMeta::Type::Integer);

        auto page = source.first(500);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows.size(), 201ull);
        QVERIFY(page.clamped[200][1]); // Cell was clamped

        auto &numCache = page.data->numeric_cache.by_column;
        QVERIFY(numCache.find(1) != numCache.end());
        QVERIFY(std::isnan(numCache.at(1)[200]));
    }
};

QTEST_GUILESS_MAIN(TestCsvFileSource)
#include "test_csv_file_source.moc"
