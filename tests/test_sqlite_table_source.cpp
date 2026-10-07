#include <QtTest>
#include <QTemporaryDir>
#include <sqlite3.h>
#include <cmath>
#include <stdexcept>
#include "parsers/sqlite_table_source.h"
#include "core/pager_state.h"
namespace {
struct VirtualTable : sqlite3_vtab {};
struct VirtualCursor : sqlite3_vtab_cursor {
    int position = 1;
};
int virtualConnect(sqlite3 *db, void *, int, const char *const *, sqlite3_vtab **table, char **) {
    const int rc =
        sqlite3_declare_vtab(db, "CREATE TABLE x(id INTEGER PRIMARY KEY,value TEXT) WITHOUT ROWID");
    if(rc != SQLITE_OK)
        return rc;
    *table = new VirtualTable{};
    return SQLITE_OK;
}
int virtualBestIndex(sqlite3_vtab *, sqlite3_index_info *) {
    return SQLITE_OK;
}
int virtualDisconnect(sqlite3_vtab *table) {
    delete static_cast<VirtualTable *>(table);
    return SQLITE_OK;
}
int virtualOpen(sqlite3_vtab *table, sqlite3_vtab_cursor **cursor) {
    auto *c = new VirtualCursor{};
    c->pVtab = table;
    *cursor = c;
    return SQLITE_OK;
}
int virtualClose(sqlite3_vtab_cursor *cursor) {
    delete static_cast<VirtualCursor *>(cursor);
    return SQLITE_OK;
}
int virtualFilter(sqlite3_vtab_cursor *cursor, int, const char *, int, sqlite3_value **) {
    static_cast<VirtualCursor *>(cursor)->position = 1;
    return SQLITE_OK;
}
int virtualNext(sqlite3_vtab_cursor *cursor) {
    ++static_cast<VirtualCursor *>(cursor)->position;
    return SQLITE_OK;
}
int virtualEof(sqlite3_vtab_cursor *cursor) {
    return static_cast<VirtualCursor *>(cursor)->position > 3;
}
int virtualColumn(sqlite3_vtab_cursor *cursor, sqlite3_context *ctx, int column) {
    const int position = static_cast<VirtualCursor *>(cursor)->position;
    if(column == 0)
        sqlite3_result_int(ctx, position);
    else
        sqlite3_result_text(ctx, "value", -1, SQLITE_STATIC);
    return SQLITE_OK;
}
int virtualRowid(sqlite3_vtab_cursor *, sqlite3_int64 *) {
    return SQLITE_ERROR;
}
int registerVirtual(sqlite3 *db, char **, const sqlite3_api_routines *) {
    static sqlite3_module module = [] {
        sqlite3_module m{};
        m.iVersion = 1;
        m.xCreate = virtualConnect;
        m.xConnect = virtualConnect;
        m.xBestIndex = virtualBestIndex;
        m.xDisconnect = virtualDisconnect;
        m.xDestroy = virtualDisconnect;
        m.xOpen = virtualOpen;
        m.xClose = virtualClose;
        m.xFilter = virtualFilter;
        m.xNext = virtualNext;
        m.xEof = virtualEof;
        m.xColumn = virtualColumn;
        m.xRowid = virtualRowid;
        return m;
    }();
    return sqlite3_create_module(db, "dtv_test_rowidless", &module, nullptr);
}
struct VirtualRegistration {
    VirtualRegistration() {
        if(sqlite3_auto_extension(reinterpret_cast<void (*)()>(registerVirtual)) != SQLITE_OK)
            throw std::runtime_error("auto extension failed");
    }
    ~VirtualRegistration() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(registerVirtual));
    }
};
struct TempFullVfs {
    sqlite3_vfs copy;
    sqlite3_vfs *original;
    static bool fail;
    static int failures;
    static sqlite3_vfs *base;
    static int open(sqlite3_vfs *, const char *name, sqlite3_file *file, int flags, int *outFlags) {
        if(fail && (flags & SQLITE_OPEN_DELETEONCLOSE)) {
            ++failures;
            return SQLITE_FULL;
        }
        return base->xOpen(base, name, file, flags, outFlags);
    }
    TempFullVfs() : original(sqlite3_vfs_find(nullptr)) {
        base = original;
        copy = *original;
        copy.zName = "dtv-test-full";
        copy.xOpen = open;
        if(sqlite3_vfs_register(&copy, 1) != SQLITE_OK)
            throw std::runtime_error("VFS registration failed");
        fail = false;
        failures = 0;
    }
    ~TempFullVfs() {
        fail = false;
        sqlite3_vfs_register(original, 1);
        sqlite3_vfs_unregister(&copy);
    }
};
bool TempFullVfs::fail = false;
int TempFullVfs::failures = 0;
sqlite3_vfs *TempFullVfs::base = nullptr;
struct Database {
    QTemporaryDir dir;
    std::string path;
    sqlite3 *db = nullptr;
    Database() : path(dir.filePath("source.db").toUtf8().toStdString()) {
        if(sqlite3_open(path.c_str(), &db) != SQLITE_OK)
            throw std::runtime_error("open failed");
    }
    ~Database() {
        sqlite3_close(db);
    }
    void exec(const std::string &sql) {
        char *error = nullptr;
        if(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message(error ? error : "SQL failed");
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }
    std::vector<int64_t> ids(const std::string &sql) {
        sqlite3_stmt *stmt = nullptr;
        if(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
        std::vector<int64_t> rows;
        int rc;
        while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
            rows.push_back(sqlite3_column_int64(stmt, 0));
        sqlite3_finalize(stmt);
        if(rc != SQLITE_DONE)
            throw std::runtime_error("query failed");
        return rows;
    }
};
std::vector<int64_t> pageIds(const dtv::core::PageResult &page) {
    std::vector<int64_t> ids;
    for(const auto &key : page.keys)
        ids.push_back(*key.rowid);
    return ids;
}
} // namespace
class TestSqliteTableSource : public QObject {
    Q_OBJECT
    private slots:
    void nullablePrimaryKeyCannotRefetch() {
        Database db;
        db.exec("CREATE TABLE t(rowid TEXT,_rowid_ TEXT,oid TEXT,k TEXT PRIMARY KEY,v TEXT); "
                "INSERT INTO t(k,v) VALUES(NULL,'first'),(NULL,'second')");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        QVERIFY(!source.canRefetch());
        auto p = source.first(2);
        QVERIFY(p.ok);
        QCOMPARE(p.data->rows.at(1).back(), std::string("second"));
        QVERIFY(p.keys.at(1).primaryKey.empty());
        QVERIFY(!source.refetch(p.keys.at(1)).ok);
    }
    void rowidlessVirtualTable() {
        VirtualRegistration registration;
        Database db;
        db.exec("CREATE VIRTUAL TABLE t USING dtv_test_rowidless");
        dtv::parsers::SqliteTableSource source;
        QVERIFY2(source.open(db.path, "t"), source.error().c_str());
        QVERIFY(!source.canSort());
        auto p = source.first(2);
        QVERIFY2(p.ok, p.error.c_str());
        QVERIFY(p.hasMore);
        QCOMPARE(p.data->rows.front().front(), std::string("1"));
        auto next = source.next(p.token, 2);
        QVERIFY(next.ok);
        QCOMPARE(next.data->rows.front().front(), std::string("3"));
        QCOMPARE(source.prev(next.token, 2).data->rows, p.data->rows);
        QVERIFY(!source.sort(0, true));
    }
    void nonAsciiEmptyMetadata() {
        Database db;
        const std::string name = "\xE4\xB8\xAD\xE6\x96\x87";
        db.exec("CREATE TABLE \"" + name + "\"(\"" + name +
                "\" INTEGER, computed TEXT GENERATED ALWAYS AS('g'))");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, name));
        QCOMPARE(source.columns().size(), size_t(2));
        QCOMPARE(source.columns().front().name, name);
        QCOMPARE(source.columns().front().type, dtv::core::ColumnMeta::Type::Integer);
        auto p = source.first(2);
        QVERIFY(p.ok);
        QVERIFY(p.data->rows.empty());
        QCOMPARE(p.data->columns.size(), size_t(2));
        QVERIFY(source.sort(1, true));
        QCOMPARE(source.rowCount().value(), int64_t(0));
    }
    void invalidSources() {
        Database db;
        db.exec("CREATE TABLE t(v INTEGER)");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(!source.open(db.path, "missing"));
        QVERIFY(!source.error().empty());
        QVERIFY(!source.open(db.path, "t\"; DROP TABLE t; --"));
        QVERIFY(source.open(db.path, "t"));
        QVERIFY(!source.refetch({}).ok);
        QVERIFY(!source.next({}, 2).ok);
        QVERIFY(!source.prev({}, 2).ok);
        QVERIFY(!source.last(2, -1).ok);
    }
    void fullTempStorageKeepsOrder() {
        TempFullVfs vfs;
        Database db;
        db.exec(
            "CREATE TABLE t(v TEXT); WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n "
            "WHERE x<120000) INSERT INTO t SELECT printf('%08d',x)||hex(zeroblob(100)) FROM n");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        QVERIFY(source.sort(0, true));
        const auto before = source.first(3);
        TempFullVfs::fail = true;
        const bool sorted = source.sort(0, false);
        TempFullVfs::fail = false;
        QVERIFY(TempFullVfs::failures > 0);
        QVERIFY(!sorted);
        QVERIFY2(source.error().find("full") != std::string::npos, source.error().c_str());
        QCOMPARE(source.first(3).data->rows, before.data->rows);
        QCOMPARE(source.rowCount().value(), int64_t(120000));
        QVERIFY(source.sort(0, false));
        QCOMPARE(source.first(3).data->rows.front().front().substr(0, 8), std::string("00120000"));
    }
    void textBoundaries_data() {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QByteArray>("display");
        QTest::addColumn<bool>("clamped");
        const QByteArray cjk("\xE4\xB8\xAD");
        const QByteArray ellipsis("\xE2\x80\xA6");
        QTest::newRow("normal") << QByteArray("hello") << QByteArray("hello") << false;
        QTest::newRow("4096") << QByteArray(4096, 'x') << QByteArray(4096, 'x') << false;
        QTest::newRow("over") << QByteArray(4097, 'x') << (QByteArray(4096, 'x') + ellipsis)
                              << true;
        QTest::newRow("cjk-ends-at-boundary") << (QByteArray(4093, 'x') + cjk + "tail")
                                              << (QByteArray(4093, 'x') + cjk + ellipsis) << true;
        QTest::newRow("embedded-null") << QByteArray("a\0b", 3) << QByteArray("a\0b", 3) << false;
    }
    void textBoundaries() {
        QFETCH(QByteArray, text);
        QFETCH(QByteArray, display);
        QFETCH(bool, clamped);
        Database db;
        db.exec("CREATE TABLE t(v TEXT)");
        sqlite3_stmt *insert = nullptr;
        QCOMPARE(sqlite3_prepare_v2(db.db, "INSERT INTO t VALUES(?1)", -1, &insert, nullptr),
                 SQLITE_OK);
        sqlite3_bind_text(insert, 1, text.data(), text.size(), SQLITE_TRANSIENT);
        QCOMPARE(sqlite3_step(insert), SQLITE_DONE);
        sqlite3_finalize(insert);
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        auto page = source.first(1);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows.front().front(), display.toStdString());
        QCOMPARE(page.clamped.front().front(), clamped);
        auto full = source.refetch(page.keys.front());
        QVERIFY(full.ok);
        QCOMPARE(full.values.front(), text.toStdString());
    }
    void emptyAndOneRowSort() {
        Database db;
        db.exec("CREATE TABLE t(v INTEGER)");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        QVERIFY(source.sort(0, true));
        QCOMPARE(source.rowCount().value(), int64_t(0));
        QVERIFY(source.first(3).data->rows.empty());
        db.exec("INSERT INTO t VALUES(42)");
        QVERIFY(source.sort(0, false));
        QCOMPARE(source.rowCount().value(), int64_t(1));
        const auto p = source.first(3);
        QCOMPARE(p.token.firstKey, int64_t(1));
        QCOMPARE(p.token.lastKey, int64_t(1));
        QCOMPARE(p.data->rows.front().front(), std::string("42"));
        QVERIFY(!p.hasMore);
    }
    void rowidlessView() {
        Database db;
        db.exec(
            "CREATE TABLE t(v); INSERT INTO t VALUES(1),(2),(3); CREATE VIEW v AS SELECT v FROM t");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "v"));
        QVERIFY(!source.canSort());
        QVERIFY(!source.canRefetch());
        auto p = source.first(2);
        QVERIFY(p.ok);
        QVERIFY(p.hasMore);
        QCOMPARE(source.next(p.token, 2).data->rows.front().front(), std::string("3"));
        QVERIFY(!source.refetch(p.keys.front()).ok);
        db.exec("DELETE FROM t");
        QVERIFY(source.open(db.path, "v"));
        QVERIFY(!source.canSort());
        QVERIFY(!source.canRefetch());
    }
    void primaryKeyStorageClasses() {
        Database db;
        db.exec("CREATE TABLE t(a BLOB,b REAL,c TEXT,PRIMARY KEY(a,b)) WITHOUT ROWID; INSERT INTO "
                "t VALUES(x'',1.25,'one'),(x'0001',2.5,'two')");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        auto p = source.first(2);
        QVERIFY(p.ok);
        for(size_t i = 0; i < p.keys.size(); ++i) {
            auto full = source.refetch(p.keys[i]);
            QVERIFY(full.ok);
            QCOMPARE(full.values, p.data->rows[i]);
        }
    }
    void pageSizesAndPager() {
        using namespace dtv::core;
        QCOMPARE(normalizePageRows(), 500);
        QCOMPARE(normalizePageRows(std::string("bad")), 500);
        QCOMPARE(normalizePageRows(std::string("500x")), 500);
        QCOMPARE(normalizePageRows(std::string("999999999999999999999")), 500);
        QCOMPARE(normalizePageRows(int64_t(-1)), 100);
        QCOMPARE(normalizePageRows(int64_t(99)), 100);
        QCOMPARE(normalizePageRows(int64_t(100)), 100);
        QCOMPARE(normalizePageRows(int64_t(3000)), 3000);
        QCOMPARE(normalizePageRows(int64_t(9000)), 3000);
        PagerState p;
        QVERIFY(!p.canFirst());
        QVERIFY(!p.canPrev());
        QVERIFY(!p.canLast());
        QVERIFY(!p.canNext());
        p.hasMore = true;
        QVERIFY(p.canNext());
        p.page = 2;
        p.hasMore = false;
        p.arrivedFromPrev = true;
        QVERIFY(p.canNext());
        QVERIFY(p.canPrev());
        p.total = 1000;
        QVERIFY(!p.canNext());
        QVERIFY(!p.canLast());
        p.total = 1001;
        QVERIFY(p.canNext());
        QVERIFY(p.canLast());
        p.total = std::numeric_limits<int64_t>::max();
        QCOMPARE(p.pages(), int64_t(18446744073709552));
        p.pageSize = -1;
        QCOMPARE(p.pages(), int64_t(0));
        p.pageSize = 0;
        QCOMPARE(p.pages(), int64_t(0));
    }
    void navigationBoundaries_data() {
        QTest::addColumn<int>("count");
        for(int count : {0, 1, 2, 3, 4, 5, 12})
            QTest::newRow(qPrintable(QString::number(count))) << count;
    }
    void navigationBoundaries() {
        QFETCH(int, count);
        Database db;
        db.exec("CREATE TABLE t(v INTEGER)");
        for(int i = 0; i < count; ++i)
            db.exec("INSERT INTO t VALUES(" + std::to_string(i) + ")");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        QVERIFY(!source.rowCount());
        auto page = source.first(3);
        QVERIFY(page.ok);
        QCOMPARE(page.data->rows.size(), size_t(std::min(count, 3)));
        QCOMPARE(page.hasMore, count > 3);
        int seen = static_cast<int>(page.data->rows.size());
        while(page.hasMore) {
            auto next = source.next(page.token, 3);
            QVERIFY(next.ok);
            QCOMPARE(source.prev(next.token, 3).data->rows, page.data->rows);
            QCOMPARE(source.next(page.token, 3).data->rows, next.data->rows);
            seen += static_cast<int>(next.data->rows.size());
            page = next;
        }
        QCOMPARE(seen, count);
        if(count) {
            auto eof = source.next(page.token, 3);
            QVERIFY(eof.ok);
            QVERIFY(eof.data->rows.empty());
            QVERIFY(!eof.hasMore);
        }
        auto tail = source.last(3, count);
        QVERIFY(tail.ok);
        QVERIFY(!tail.hasMore);
        QCOMPARE(tail.data->rows.size(), size_t(count ? (count % 3 ? count % 3 : 3) : 0));
        if(count)
            QCOMPARE(tail.data->rows.back().at(0), std::to_string(count - 1));
        QCOMPARE(source.first(3).data->rows.size(), size_t(std::min(count, 3)));
        QVERIFY(!source.first(0).ok);
        QVERIFY(!source.first(3001).ok);
    }
    void aliases_data() {
        QTest::addColumn<QString>("definition");
        QTest::addColumn<bool>("sortable");
        QTest::newRow("ordinary") << "v TEXT" << true;
        QTest::newRow("rowid") << "ROWID TEXT, v TEXT" << true;
        QTest::newRow("two") << "rowid TEXT, _ROWID_ TEXT, v TEXT" << true;
        QTest::newRow("all") << "rowid TEXT, _rowid_ TEXT, oid TEXT, v TEXT" << false;
    }
    void aliases() {
        QFETCH(QString, definition);
        QFETCH(bool, sortable);
        Database db;
        db.exec("CREATE TABLE t(" + definition.toStdString() + ")");
        db.exec("INSERT INTO t(v) VALUES('one'),('two'),('three')");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        QCOMPARE(source.canSort(), sortable);
        QCOMPARE(source.canRefetch(), sortable);
        QVERIFY(!source.canSearch());
        auto p = source.first(2);
        QVERIFY(p.ok);
        QVERIFY(p.hasMore);
        QCOMPARE(source.next(p.token, 2).data->rows.back().back(), std::string("three"));
        if(!sortable)
            QVERIFY(!source.sort(0, true));
    }
    void withoutRowidAndRefetch() {
        Database db;
        db.exec("CREATE TABLE t(a TEXT,b INTEGER,value TEXT,PRIMARY KEY(b,a)) WITHOUT ROWID");
        db.exec("INSERT INTO t VALUES('a',1,'one'),('b',2,'two'),('c',3,'three')");
        sqlite3_stmt *probe = nullptr;
        QCOMPARE(sqlite3_prepare_v2(db.db, "SELECT rowid FROM t", -1, &probe, nullptr),
                 SQLITE_ERROR);
        sqlite3_finalize(probe);
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        QVERIFY(!source.canSort());
        QVERIFY(source.canRefetch());
        auto p = source.first(2);
        QVERIFY(p.ok);
        QVERIFY(p.hasMore);
        QCOMPARE(p.keys.front().primaryKey.size(), size_t(2));
        auto next = source.next(p.token, 2);
        QCOMPARE(next.data->rows.front().back(), std::string("three"));
        QCOMPARE(source.prev(next.token, 2).data->rows, p.data->rows);
        QCOMPARE(source.last(2, 3).data->rows, next.data->rows);
        auto full = source.refetch(next.keys.front());
        QVERIFY(full.ok);
        QCOMPARE(full.values.back(), std::string("three"));
        QVERIFY(!source.sort(0, true));
        db.exec("DELETE FROM t WHERE b=3");
        QVERIFY(!source.refetch(next.keys.front()).ok);
    }
    void metadataAndCells() {
        Database db;
        db.exec("CREATE TABLE \"t\"\"x\"(i INTEGER,r REAL,d DOUBLE,b BOOL,u,\"quoted\"\"col\" "
                "TEXT,g TEXT GENERATED ALWAYS AS (i || 'g'))");
        const std::string cjk = "\xE4\xB8\xAD";
        std::string large(4095, 'x');
        large += cjk;
        large += "tail";
        sqlite3_stmt *insert = nullptr;
        QCOMPARE(sqlite3_prepare_v2(db.db,
                                    "INSERT INTO \"t\"\"x\" VALUES(1,2.5,3.5,1,x'010203',?1)", -1,
                                    &insert, nullptr),
                 SQLITE_OK);
        sqlite3_bind_text(insert, 1, large.data(), static_cast<int>(large.size()),
                          SQLITE_TRANSIENT);
        QCOMPARE(sqlite3_step(insert), SQLITE_DONE);
        sqlite3_finalize(insert);
        dtv::parsers::SqliteTableSource source;
        QVERIFY2(source.open(db.path, "t\"x"), source.error().c_str());
        QCOMPARE(source.columns().size(), size_t(7));
        using Type = dtv::core::ColumnMeta::Type;
        QCOMPARE(source.columns()[0].type, Type::Integer);
        QCOMPARE(source.columns()[1].type, Type::Float);
        QCOMPARE(source.columns()[2].type, Type::Float);
        QCOMPARE(source.columns()[3].type, Type::Boolean);
        QCOMPARE(source.columns()[4].type, Type::String);
        QCOMPARE(source.columns()[5].name, std::string("quoted\"col"));
        auto p = source.first(2);
        QVERIFY(p.ok);
        QCOMPARE(p.data->rows.front()[4], std::string("[BLOB 3 bytes]"));
        QCOMPARE(p.data->rows.front()[5], std::string(4095, 'x') + "\xE2\x80\xA6");
        QVERIFY(p.clamped.front()[5]);
        QCOMPARE(p.data->rows.front()[6], std::string("1g"));
        auto full = source.refetch(p.keys.front());
        QVERIFY(full.ok);
        QCOMPARE(full.values[5], large);
        QCOMPARE(p.data->numeric_cache.by_column.at(1).front(), 2.5);
        db.exec("DELETE FROM \"t\"\"x\"; INSERT INTO \"t\"\"x\" "
                "VALUES(NULL,NULL,NULL,NULL,NULL,'short')");
        p = source.first(2);
        QVERIFY(!p.clamped.front()[5]);
        QVERIFY(std::isnan(p.data->numeric_cache.by_column.at(0).front()));
    }
    void numericCacheStorageAndReverse() {
        Database db;
        db.exec("CREATE TABLE t(v REAL); INSERT INTO t VALUES(1.5),(NULL),('bad'),(x'01'),(2.75)");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        auto first = source.first(3);
        QVERIFY(first.ok);
        const auto &cache = first.data->numeric_cache.by_column.at(0);
        QCOMPARE(cache.size(), size_t(3));
        QCOMPARE(cache[0], 1.5);
        QVERIFY(std::isnan(cache[1]));
        QVERIFY(std::isnan(cache[2]));
        auto next = source.next(first.token, 3);
        QVERIFY(next.ok);
        QVERIFY(std::isnan(next.data->numeric_cache.by_column.at(0)[0]));
        QCOMPARE(next.data->numeric_cache.by_column.at(0)[1], 2.75);
        auto prev = source.prev(next.token, 3);
        QVERIFY(prev.ok);
        QCOMPARE(prev.data->numeric_cache.by_column.at(0)[0], 1.5);
        QVERIFY(std::isnan(prev.data->numeric_cache.by_column.at(0)[2]));
    }
    void sortingMatchesSqlite() {
        Database db;
        db.exec("CREATE TABLE t(v); INSERT INTO t "
                "VALUES(NULL),(3),(1),(3),('a'),(x'0102'),(2.5),('10')");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        for(bool asc : {true, false, true}) {
            QVERIFY2(source.sort(0, asc), source.error().c_str());
            QCOMPARE(source.rowCount().value(), db.ids("SELECT COUNT(*) FROM t").front());
            const std::string direction = asc ? " ASC" : " DESC";
            auto reference =
                db.ids("SELECT rowid FROM t ORDER BY v" + direction + ",rowid" + direction);
            std::vector<int64_t> ids;
            auto p = source.first(3);
            QCOMPARE(p.token.firstKey, int64_t(1));
            for(;;) {
                QVERIFY(p.ok);
                auto part = pageIds(p);
                ids.insert(ids.end(), part.begin(), part.end());
                if(!p.hasMore)
                    break;
                auto next = source.next(p.token, 3);
                QCOMPARE(source.prev(next.token, 3).data->rows, p.data->rows);
                p = next;
            }
            QCOMPARE(ids, reference);
            auto tail = source.last(3, 8);
            QCOMPARE(pageIds(tail), std::vector<int64_t>(reference.end() - 2, reference.end()));
            const auto before = source.first(3);
            QVERIFY(!source.sort(99, true));
            QCOMPARE(source.first(3).data->rows, before.data->rows);
            QVERIFY(!source.sort(0, !asc, [] {
                return true;
            }));
            QCOMPARE(source.first(3).data->rows, before.data->rows);
        }
    }
    void cancelledBuildKeepsOrder() {
        Database db;
        db.exec("CREATE TABLE t(v INTEGER); WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 "
                "FROM n WHERE x<30000) INSERT INTO t SELECT x FROM n");
        dtv::parsers::SqliteTableSource source;
        QVERIFY(source.open(db.path, "t"));
        QVERIFY(source.sort(0, true));
        const auto before = source.first(3);
        int polls = 0;
        QVERIFY(!source.sort(0, false, [&] {
            return ++polls > 5;
        }));
        QVERIFY(polls > 5);
        QCOMPARE(source.first(3).data->rows, before.data->rows);
        QVERIFY(source.sort(0, false));
        QCOMPARE(source.first(3).data->rows.front().front(), std::string("30000"));
        source.setCancelCheck([] {
            return true;
        });
        QVERIFY(!source.first(3).ok);
        source.setCancelCheck({});
        QVERIFY(source.first(3).ok);
    }
    void materializedAdapter() {
        auto data = std::make_shared<dtv::core::TableData>();
        data->rows = {{"a"}, {"b"}};
        dtv::core::MaterializedTableSource source(data);
        auto p = source.first(1);
        QVERIFY(p.ok);
        QCOMPARE(p.data.get(), data.get());
        QCOMPARE(p.data->rows.size(), size_t(2));
        QCOMPARE(source.rowCount().value(), int64_t(2));
        data->truncated = true;
        dtv::core::MaterializedTableSource partial(data);
        QVERIFY(!partial.rowCount());
        partial.setKnownTotal(20);
        QCOMPARE(partial.rowCount().value(), int64_t(20));
        QVERIFY(!source.canRefetch());
        QVERIFY(!source.canSort());
    }
    void navigation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("test.db").toUtf8().toStdString();
        sqlite3 *db = nullptr;
        QCOMPARE(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        const int rc = sqlite3_exec(db,
                                    "CREATE TABLE t(v TEXT); INSERT INTO t(rowid,v) "
                                    "VALUES(-9,'a'),(4,'b'),(9000000000,'c')",
                                    nullptr, nullptr, nullptr);
        sqlite3_close(db);
        QCOMPARE(rc, SQLITE_OK);
        dtv::parsers::SqliteTableSource source;
        QVERIFY2(source.open(path, "t"), source.error().c_str());
        auto page = source.first(2);
        QVERIFY2(page.ok, page.error.c_str());
        QCOMPARE(page.data->rows.size(), size_t(2));
        QVERIFY(page.hasMore);
        QCOMPARE(page.token.firstKey, int64_t(-9));
        auto tail = source.next(page.token, 2);
        QVERIFY(tail.ok);
        QCOMPARE(tail.data->rows.at(0).at(0), std::string("c"));
        QVERIFY(!tail.hasMore);
        auto back = source.prev(tail.token, 2);
        QVERIFY(back.ok);
        QCOMPARE(back.data->rows, page.data->rows);
        QVERIFY(!source.last(2, std::nullopt).ok);
        source.setKnownTotal(3);
        auto last = source.last(2, 3);
        QVERIFY(last.ok);
        QCOMPARE(last.data->rows, tail.data->rows);
    }
};
QTEST_GUILESS_MAIN(TestSqliteTableSource)
#include "test_sqlite_table_source.moc"
