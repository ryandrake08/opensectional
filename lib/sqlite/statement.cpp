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
                throw error("Failed to bind parameter " + std::to_string(index), sqlite3_db_handle(stmt));
            }
        }
    } // namespace

    void statement::release::operator()(sqlite3_stmt* stmt) const
    {
        sqlite3_finalize(stmt);
    }

    statement::statement(sqlite3_stmt* stmt) : stmt_(stmt)
    {
    }

    void statement::reset()
    {
        sqlite3_reset(stmt_.get());
    }

    void statement::bind_null(int index)
    {
        check_bind(sqlite3_bind_null(stmt_.get(), index), index, stmt_.get());
    }

    void statement::bind(int index, int value)
    {
        check_bind(sqlite3_bind_int(stmt_.get(), index, value), index, stmt_.get());
    }

    void statement::bind(int index, std::int64_t value)
    {
        check_bind(sqlite3_bind_int64(stmt_.get(), index, value), index, stmt_.get());
    }

    void statement::bind(int index, double value)
    {
        check_bind(sqlite3_bind_double(stmt_.get(), index, value), index, stmt_.get());
    }

    void statement::bind(int index, const char* value)
    {
        check_bind(sqlite3_bind_text(stmt_.get(), index, value, -1, SQLITE_TRANSIENT), index, stmt_.get());
    }

    void statement::bind(int index, const std::string& value)
    {
        check_bind(sqlite3_bind_text(stmt_.get(), index, value.c_str(), -1, SQLITE_TRANSIENT), index, stmt_.get());
    }

    void statement::bind(int index, const std::vector<int>& values)
    {
        check_bind(sqlite3_bind_blob(stmt_.get(), index, values.data(), static_cast<int>(values.size() * sizeof(int)),
                                     SQLITE_TRANSIENT),
                   index, stmt_.get());
    }

    void statement::bind(int index, const std::vector<double>& values)
    {
        check_bind(sqlite3_bind_blob(stmt_.get(), index, values.data(),
                                     static_cast<int>(values.size() * sizeof(double)), SQLITE_TRANSIENT),
                   index, stmt_.get());
    }

    bool statement::step()
    {
        int rc = sqlite3_step(stmt_.get());
        if(rc == SQLITE_ROW)
        {
            return true;
        }
        if(rc == SQLITE_DONE)
        {
            return false;
        }
        throw error("sqlite3_step failed", sqlite3_db_handle(stmt_.get()));
    }

    int statement::column_count() const
    {
        return sqlite3_column_count(stmt_.get());
    }

    int statement::column_int(int col)
    {
        return sqlite3_column_int(stmt_.get(), col);
    }

    std::int64_t statement::column_int64(int col)
    {
        return sqlite3_column_int64(stmt_.get(), col);
    }

    double statement::column_double(int col)
    {
        return sqlite3_column_double(stmt_.get(), col);
    }

    bool statement::column_is_null(int col)
    {
        return sqlite3_column_type(stmt_.get(), col) == SQLITE_NULL;
    }

    std::string statement::column_text(int col)
    {
        const unsigned char* text = sqlite3_column_text(stmt_.get(), col);
        return text ? std::string(reinterpret_cast<const char*>(text)) : std::string();
    }

} // namespace sqlite
