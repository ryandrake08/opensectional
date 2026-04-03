#pragma once

#include <memory>
#include <string>
#include <vector>

struct sqlite3_stmt;

namespace sqlite
{
    class database;

    class statement
    {
        friend class database;

        struct impl;
        std::unique_ptr<impl> pimpl;

        explicit statement(sqlite3_stmt* stmt);

    public:
        ~statement();

        // Non-copyable
        statement(const statement&) = delete;
        statement& operator=(const statement&) = delete;

        // Moveable
        statement(statement&& other) noexcept;
        statement& operator=(statement&& other) noexcept;

        // Reset for re-execution with new bindings
        void reset();

        // Bind parameters (1-indexed)
        void bind(int index, int value);
        void bind(int index, double value);
        void bind(int index, const char* value);
        void bind(int index, const std::string& value);
        void bind(int index, const std::vector<int>& values);
        void bind(int index, const std::vector<double>& values);

        // Step to next row; returns true if a row is available
        bool step();

        // Column accessors (0-indexed)
        int column_int(int col);
        double column_double(int col);
        std::string column_text(int col);
    };

} // namespace sqlite
