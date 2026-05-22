#include "error.hpp"
#include <sqlite3.h>

namespace sqlite
{
    error::error(const std::string& message, sqlite3* db)
        : std::runtime_error(message + ": " + sqlite3_errmsg(db))
    {
    }
} // namespace sqlite
