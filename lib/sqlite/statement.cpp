#include "statement.hpp"
#include "error.hpp"
#include <sqlite3.h>
#include <string>

namespace sqlite
{
    namespace
    {
        // sqlite3_bind_* returns SQLITE_OK or, most commonly, SQLITE_RANGE for
        // an out-of-range parameter index; left unchecked that misbinding is
        // silent and the query runs with the parameter still NULL.
        void check_bind(int rc, int index, sqlite3_stmt* stmt)
        {
            if(rc != SQLITE_OK)
            {
                throw error("Failed to bind parameter " + std::to_string(index),
                            sqlite3_db_handle(stmt));
            }
        }
    } // namespace

    struct statement::impl
    {
        sqlite3_stmt* stmt;

        explicit impl(sqlite3_stmt* s) : stmt(s)
        {
        }

        ~impl()
        {
            sqlite3_finalize(stmt);
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    statement::statement(sqlite3_stmt* stmt) : pimpl(new impl(stmt))
    {
    }

    statement::~statement() = default;
    statement::statement(statement&& other) noexcept = default;
    statement& statement::operator=(statement&& other) noexcept = default;

    void statement::reset()
    {
        sqlite3_reset(pimpl->stmt);
    }

    void statement::bind_null(int index)
    {
        check_bind(sqlite3_bind_null(pimpl->stmt, index), index, pimpl->stmt);
    }

    void statement::bind(int index, int value)
    {
        check_bind(sqlite3_bind_int(pimpl->stmt, index, value), index, pimpl->stmt);
    }

    void statement::bind(int index, std::int64_t value)
    {
        check_bind(sqlite3_bind_int64(pimpl->stmt, index, value), index, pimpl->stmt);
    }

    void statement::bind(int index, double value)
    {
        check_bind(sqlite3_bind_double(pimpl->stmt, index, value), index, pimpl->stmt);
    }

    void statement::bind(int index, const char* value)
    {
        check_bind(sqlite3_bind_text(pimpl->stmt, index, value, -1, SQLITE_TRANSIENT), index, pimpl->stmt);
    }

    void statement::bind(int index, const std::string& value)
    {
        check_bind(sqlite3_bind_text(pimpl->stmt, index, value.c_str(), -1, SQLITE_TRANSIENT), index,
                   pimpl->stmt);
    }

    void statement::bind(int index, const std::vector<int>& values)
    {
        check_bind(sqlite3_bind_blob(pimpl->stmt, index, values.data(),
                                     static_cast<int>(values.size() * sizeof(int)), SQLITE_TRANSIENT),
                   index, pimpl->stmt);
    }

    void statement::bind(int index, const std::vector<double>& values)
    {
        check_bind(sqlite3_bind_blob(pimpl->stmt, index, values.data(),
                                     static_cast<int>(values.size() * sizeof(double)), SQLITE_TRANSIENT),
                   index, pimpl->stmt);
    }

    bool statement::step()
    {
        int rc = sqlite3_step(pimpl->stmt);
        if(rc == SQLITE_ROW)
        {
            return true;
        }
        if(rc == SQLITE_DONE)
        {
            return false;
        }
        throw error("sqlite3_step failed", sqlite3_db_handle(pimpl->stmt));
    }

    int statement::column_count() const
    {
        return sqlite3_column_count(pimpl->stmt);
    }

    int statement::column_int(int col)
    {
        return sqlite3_column_int(pimpl->stmt, col);
    }

    std::int64_t statement::column_int64(int col)
    {
        return sqlite3_column_int64(pimpl->stmt, col);
    }

    double statement::column_double(int col)
    {
        return sqlite3_column_double(pimpl->stmt, col);
    }

    std::string statement::column_text(int col)
    {
        const unsigned char* text = sqlite3_column_text(pimpl->stmt, col);
        return text ? std::string(reinterpret_cast<const char*>(text)) : std::string();
    }

} // namespace sqlite
