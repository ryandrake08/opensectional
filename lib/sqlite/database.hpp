#pragma once

#include <memory>
#include <string>

namespace sqlite
{
    class statement;

    class database
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // Open database at path (read-only by default)
        explicit database(const char* path, bool read_only = true);
        ~database();

        // Non-copyable
        database(const database&) = delete;
        database& operator=(const database&) = delete;

        // Moveable
        database(database&& other) noexcept;
        database& operator=(database&& other) noexcept;

        // Prepare a statement against this database
        statement prepare(const char* sql);

        // Execute one or more SQL statements with no result rows. Wraps
        // sqlite3_exec — accepts multi-statement scripts (CREATE / DROP /
        // BEGIN / COMMIT). Throws std::runtime_error on any SQLite error.
        // For repeated parameterized work (INSERT loops, parameterized
        // SELECT), use prepare() instead.
        void exec(const char* sql);

        // Get the last error message
        std::string error_message() const;
    };

} // namespace sqlite
