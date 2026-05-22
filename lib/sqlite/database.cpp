#include "database.hpp"
#include "error.hpp"
#include "statement.hpp"
#include <sqlite3.h>

namespace sqlite
{
    struct database::impl
    {
        sqlite3* db = nullptr;

        impl(const char* path, bool read_only)
        {
            int flags = read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
            int rc = sqlite3_open_v2(path, &db, flags, nullptr);
            if(rc != SQLITE_OK)
            {
                error err("Failed to open database", db);
                sqlite3_close_v2(db);
                throw err;
            }
            // Performance hints: mmap the file (faster than pread on
            // macOS), keep more index pages resident across queries,
            // and put any temp B-trees in RAM. All three soft-fail
            // to defaults if SQLite can't honor them.
            sqlite3_exec(db, "PRAGMA mmap_size = 268435456", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA cache_size = -32000", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "PRAGMA temp_store = MEMORY", nullptr, nullptr, nullptr);
        }

        ~impl()
        {
            sqlite3_close_v2(db);
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    database::database(const char* path, bool read_only) : pimpl(new impl(path, read_only))
    {
    }

    database::~database() = default;
    database::database(database&& other) noexcept = default;
    database& database::operator=(database&& other) noexcept = default;

    statement database::prepare(const char* sql)
    {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(pimpl->db, sql, -1, &stmt, nullptr);
        if(rc != SQLITE_OK)
        {
            throw error("Failed to prepare statement", pimpl->db);
        }
        return statement(stmt);
    }

    void database::exec(const char* sql)
    {
        int rc = sqlite3_exec(pimpl->db, sql, nullptr, nullptr, nullptr);
        if(rc != SQLITE_OK)
        {
            throw error("Failed to exec SQL", pimpl->db);
        }
    }

    std::int64_t database::last_insert_rowid() const
    {
        return sqlite3_last_insert_rowid(pimpl->db);
    }

} // namespace sqlite
