#include "data_source.hpp"

namespace osect
{
    data_source_status data_source::status_at(std::chrono::system_clock::time_point now) const
    {
        if(!expires)
        {
            return data_source_status::unknown;
        }
        return now < *expires ? data_source_status::fresh : data_source_status::expired;
    }

    data_source_status data_source::status() const
    {
        return status_at(std::chrono::system_clock::now());
    }
}
