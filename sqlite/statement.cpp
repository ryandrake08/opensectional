#include "statement.hpp"
#include <sqlite3.h>
#include <string>

namespace sqlite
{
    struct statement::impl
    {
        sqlite3_stmt* stmt;

        explicit impl(sqlite3_stmt* s)
            : stmt(s)
        {
        }

        ~impl()
        {
            sqlite3_finalize(stmt);
        }
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

    void statement::bind(int index, int value)
    {
        sqlite3_bind_int(pimpl->stmt, index, value);
    }

    void statement::bind(int index, double value)
    {
        sqlite3_bind_double(pimpl->stmt, index, value);
    }

    void statement::bind(int index, const char* value)
    {
        sqlite3_bind_text(pimpl->stmt, index, value, -1, SQLITE_TRANSIENT);
    }

    void statement::bind(int index, const std::string& value)
    {
        sqlite3_bind_text(pimpl->stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }

    void statement::bind(int index, const std::vector<int>& values)
    {
        sqlite3_bind_blob(pimpl->stmt, index, values.data(),
                          static_cast<int>(values.size() * sizeof(int)), SQLITE_TRANSIENT);
    }

    void statement::bind(int index, const std::vector<double>& values)
    {
        sqlite3_bind_blob(pimpl->stmt, index, values.data(),
                          static_cast<int>(values.size() * sizeof(double)), SQLITE_TRANSIENT);
    }

    bool statement::step()
    {
        return sqlite3_step(pimpl->stmt) == SQLITE_ROW;
    }

    int statement::column_int(int col)
    {
        return sqlite3_column_int(pimpl->stmt, col);
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
