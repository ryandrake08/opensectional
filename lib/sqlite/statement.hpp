#pragma once

#include <cstdint>
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
        void bind_null(int index);
        void bind(int index, int value);
        void bind(int index, std::int64_t value);
        void bind(int index, double value);
        void bind(int index, const char* value);
        void bind(int index, const std::string& value);
        void bind(int index, const std::vector<int>& values);
        void bind(int index, const std::vector<double>& values);

        // Step to next row; returns true if a row is available, false
        // when the result set is exhausted. A statement parked on a row
        // holds an implicit read transaction open until reset() — single-
        // row reads (`if (step()) { ...read columns... }`) must call
        // reset() before returning, or the connection will be pinned to
        // that snapshot and miss writes made by other connections.
        bool step();

        // Number of columns in the result set
        int column_count() const;

        // Column accessors (0-indexed)
        int column_int(int col);
        std::int64_t column_int64(int col);
        double column_double(int col);
        bool column_is_null(int col);
        std::string column_text(int col);
    };

} // namespace sqlite
