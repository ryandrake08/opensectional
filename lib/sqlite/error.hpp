#pragma once

#include <stdexcept>
#include <string>

struct sqlite3;

namespace sqlite
{
    // Exception that automatically appends sqlite3_errmsg() to the message.
    class error : public std::runtime_error
    {
    public:
        error(const std::string& message, sqlite3* db);
    };

} // namespace sqlite
