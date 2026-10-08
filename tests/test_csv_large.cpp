#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QElapsedTimer>
#include <memory>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#endif
#endif

#include "parsers/csv_file_source.h"
#include "core/table_source.h"
#include "core/table_data.h"

class TestCsvLarge : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
    QString m_largeCsvPath;
    qint64 m_fileSize = 0;

    size_t getProcessPrivateBytes() const
    {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            return pmc.PrivateUsage;
        }
#endif
        return 0;
    }

private slots:
    void initTestCase()
    {
        m_tempDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_tempDir->isValid());
        m_largeCsvPath = m_tempDir->filePath("large_1m.csv");

        QFile file(m_largeCsvPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QTextStream out(&file);

        // Header
        out << "id,name,value,notes\n";

        // Generate 1,000,000 rows with varying data, including quoted multiline fields
        // Approximately 75-80 bytes per row -> ~75-80 MiB file (> 64 MiB limit)
        for (int i = 1; i <= 1000000; ++i) {
            if (i % 1000 == 0) {
                // Quoted multiline field
                out << i << ",\"Item_" << i << "\"," << (i * 1.5)
                    << ",\"Note line 1\nNote line 2 for item " << i << "\"\n";
            } else if (i % 250 == 0) {
                // Quoted doubled quote
                out << i << ",\"Item \"\"" << i << "\"\"\"," << (i * 1.5) << ",\"normal note\"\n";
            } else {
                out << i << ",Item_" << i << "," << (i * 1.5) << ",some_filler_padding_note_long_extra_bytes_for_size\n";
            }
        }
        file.close();

        m_fileSize = QFileInfo(m_largeCsvPath).size();
        qInfo("Generated 1,000,000 rows CSV: %lld bytes (%.2f MiB)",
              m_fileSize, m_fileSize / (1024.0 * 1024.0));
        QVERIFY(m_fileSize > 64 * 1024 * 1024); // > 64 MiB
    }

    void cleanupTestCase()
    {
        m_tempDir.reset();
    }

    void testLargeCsvFirstPageLatencyAndMemoryPlateau()
    {
        dtv::parsers::CsvFileSource source;

        QElapsedTimer openTimer;
        openTimer.start();
        QVERIFY(source.open(m_largeCsvPath.toStdString()));
        qint64 openMs = openTimer.elapsed();
        qInfo("Large CSV open() elapsed: %lld ms", openMs);

        // Benchmark first page latency (< 500 ms target)
        QElapsedTimer firstPageTimer;
        firstPageTimer.start();
        auto p1 = source.first(500);
        qint64 firstPageMs = firstPageTimer.elapsed();
        qInfo("First page fetch elapsed: %lld ms", firstPageMs);

        QVERIFY(p1.ok);
        QCOMPARE(p1.data->rows.size(), 500ull);
        QCOMPARE(p1.data->rows[0][0], std::string("1"));
        // Verify doubled-quotes unescaping on page 1 (row 250)
        QCOMPARE(p1.data->rows[249][0], std::string("250"));
        QCOMPARE(p1.data->rows[249][1], std::string("Item \"250\""));
        QVERIFY(firstPageMs < 500); // Latency target < 500 ms

        // Advance indexing to completion
        QElapsedTimer indexTimer;
        indexTimer.start();
        int slices = 0;
        while (true) {
            auto progress = source.advanceIndex(256 * 1024);
            slices++;
            QVERIFY(progress.error.empty());
            if (progress.isComplete) {
                break;
            }
        }
        qint64 indexMs = indexTimer.elapsed();
        qInfo("Indexed 1,000,000 rows in %lld ms across %d slices", indexMs, slices);

        QVERIFY(source.rowCount().has_value());
        QCOMPARE(*source.rowCount(), 1000000LL);

        // Benchmark page turn latency (< 50 ms target)
        dtv::core::PageToken token = p1.token;
        size_t memBefore = getProcessPrivateBytes();
        qint64 totalTurnMs = 0;

        for (int p = 2; p <= 50; ++p) {
            QElapsedTimer turnTimer;
            turnTimer.start();
            auto nextResult = source.next(token, 500);
            qint64 turnMs = turnTimer.elapsed();
            totalTurnMs += turnMs;

            QVERIFY(nextResult.ok);
            QCOMPARE(nextResult.data->rows.size(), 500ull);
            QVERIFY(turnMs < 500); // Safety ceiling per page turn

            if (p == 2) {
                // Verify multiline quoted field decoding across row on page 2 (row 1000)
                QCOMPARE(nextResult.data->rows[499][0], std::string("1000"));
                QCOMPARE(nextResult.data->rows[499][3],
                         std::string("Note line 1\nNote line 2 for item 1000"));
            }

            token = nextResult.token;
        }

        qint64 avgTurnMs = totalTurnMs / 49;
        qInfo("Average page turn latency: %lld ms", avgTurnMs);
        QVERIFY(avgTurnMs < 50); // Indexed page-turn benchmark < 50 ms

        // Verify prev() navigation on large indexed source
        auto prevResult = source.prev(token, 500);
        QVERIFY(prevResult.ok);
        QCOMPARE(prevResult.data->rows.size(), 500ull);
        QCOMPARE(prevResult.data->rows[0][0], std::string("24001"));

        size_t memAfter = getProcessPrivateBytes();
        qInfo("Private bytes: before turns = %zu KB, after turns = %zu KB",
              memBefore / 1024, memAfter / 1024);

        if (memBefore > 0 && memAfter > 0) {
            // Verify memory plateau: page turns must not leak (> 16 MiB growth)
            QVERIFY(memAfter <= memBefore + 16 * 1024 * 1024);
        }

        // Last page check
        auto pLast = source.last(500, 1000000);
        QVERIFY(pLast.ok);
        QCOMPARE(pLast.data->rows.size(), 500ull);
        QCOMPARE(pLast.data->rows.back()[0], std::string("1000000"));
        QCOMPARE(pLast.data->rows.back()[3],
                 std::string("Note line 1\nNote line 2 for item 1000000"));
        QVERIFY(!pLast.hasMore);
    }
};

QTEST_GUILESS_MAIN(TestCsvLarge)
#include "test_csv_large.moc"
