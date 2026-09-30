#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct sqlite3;

namespace sqlite
{
    class statement;

    class database
    {
        // Closes the connection.
        struct release
        {
            void operator()(sqlite3* db) const;
        };

        std::unique_ptr<sqlite3, release> db_;

    public:
        // Open database at path (read-only by default)
        explicit database(const char* path, bool read_only = true);

        // Non-copyable
        database(const database&) = delete;
        database& operator=(const database&) = delete;

        // Moveable
        database(database&& other) noexcept = default;
        database& operator=(database&& other) noexcept = default;

        // Prepare a statement against this database
        statement prepare(const char* sql);

        // Execute one or more SQL statements with no result rows. Wraps
        // sqlite3_exec — accepts multi-statement scripts (CREATE / DROP /
        // BEGIN / COMMIT). Throws std::runtime_error on any SQLite error.
        // For repeated parameterized work (INSERT loops, parameterized
        // SELECT), use prepare() instead.
        void exec(const char* sql);

        // rowid of the row inserted by the most recent INSERT on this
        // connection. Wraps sqlite3_last_insert_rowid; intended use is
        // capturing INTEGER PRIMARY KEY AUTOINCREMENT ids right after step().
        std::int64_t last_insert_rowid() const;
    };

} // namespace sqlite
