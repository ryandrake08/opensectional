#include "ephemeral_data.hpp"

#include "ephemeral_cache.hpp"
#include "http_client.hpp"
#include "tfr_source.hpp"

namespace osect
{
    struct ephemeral_data::impl
    {
        http_client http;
        ephemeral_cache cache;
        tfr_source tfrs;

        // Last-seen last_updated() per source, for poll_advance().
        std::optional<std::chrono::system_clock::time_point> tfrs_seen;

        explicit impl(bool offline)
            : http(offline)
            , tfrs(http, cache)
        {
        }
    };

    ephemeral_data::ephemeral_data(bool offline)
        : pimpl(std::make_unique<impl>(offline))
    {
    }

    ephemeral_data::~ephemeral_data() = default;

    const tfr_source& ephemeral_data::tfrs() const
    {
        return pimpl->tfrs;
    }

    std::vector<data_source> ephemeral_data::as_data_sources() const
    {
        return { pimpl->tfrs.as_data_source() };
    }

    bool ephemeral_data::poll_advance()
    {
        bool advanced = false;
        const auto cur = pimpl->tfrs.last_updated();
        if(cur != pimpl->tfrs_seen)
        {
            pimpl->tfrs_seen = cur;
            advanced = true;
        }
        return advanced;
    }
}
