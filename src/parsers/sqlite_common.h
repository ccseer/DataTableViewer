#pragma once

#include <sqlite3.h>

#include <memory>
#include <mutex>
#include <string>

namespace dtv::parsers {

class InterruptHandle {
public:
    InterruptHandle() = default;
    ~InterruptHandle() = default;

    InterruptHandle(const InterruptHandle &) = delete;
    InterruptHandle &operator=(const InterruptHandle &) = delete;

    uint64_t interrupt() {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_sequence;
        if(m_db) {
            sqlite3_interrupt(m_db);
        }
        return m_sequence;
    }

    void setDb(sqlite3 *db) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_db = db;
    }

    void clear() {
        setDb(nullptr);
    }

    sqlite3 *db() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_db;
    }

    uint64_t sequence() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sequence;
    }

private:
    mutable std::mutex m_mutex;
    sqlite3 *m_db = nullptr;
    uint64_t m_sequence = 0;
};

struct SqliteOpenResult {
    sqlite3 *db = nullptr;
    std::string error;
};

std::string escapeSqlIdentifier(const std::string &name);
std::string mapSqliteDeclType(const char *decl_type);
SqliteOpenResult openReadOnly(const std::string &path);

} // namespace dtv::parsers
