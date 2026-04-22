#include "database.hpp"
#include "statement.hpp"
#include <sqlite3.h>
#include <stdexcept>

namespace sqlite
{
    struct database::impl
    {
        sqlite3* db = nullptr;

        impl(const char* path, bool read_only)
        {
            int flags = read_only ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
            int rc = sqlite3_open_v2(path, &db, flags, nullptr);
            if (rc != SQLITE_OK)
            {
                std::string msg = "Failed to open database: ";
                msg += sqlite3_errmsg(db);
                sqlite3_close(db);
                throw std::runtime_error(msg);
            }
        }

        ~impl()
        {
            sqlite3_close(db);
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
        if (rc != SQLITE_OK)
        {
            std::string msg = "Failed to prepare statement: ";
            msg += sqlite3_errmsg(pimpl->db);
            throw std::runtime_error(msg);
        }
        return statement(stmt);
    }

    std::string database::error_message() const
    {
        return sqlite3_errmsg(pimpl->db);
    }

} // namespace sqlite
