#pragma once

#include <chrono>
#include <memory>
#include <string>

namespace osect
{
    // Synchronous HTTPS client used by ephemeral data sources. Wraps a
    // single libcurl easy handle whose TLS/connection state is reused
    // across `get()` calls — construction allocates the handle, the
    // destructor releases it. Not thread-safe; each background-thread
    // source owns its own instance.
    //
    // HTTP-level errors (4xx / 5xx, including 304) populate the response
    // normally and never throw. Transport-level errors (DNS failure,
    // connection refused, TLS failure, timeout) throw std::runtime_error
    // with the curl-supplied diagnostic — the source catches and falls
    // back to its on-disk cache.
    class http_client
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        http_client();
        ~http_client();

        http_client(const http_client&) = delete;
        http_client& operator=(const http_client&) = delete;

        struct request
        {
            std::string url;
            // ETag from a prior response, sent as If-None-Match. Empty
            // for the first request of a given resource.
            std::string if_none_match;
            std::chrono::seconds timeout{30};
            std::string user_agent{"OpenSectional/0.1.0 (+https://existens.org)"};
        };

        struct response
        {
            int status_code = 0;
            std::string body;            // gzip-decompressed by curl
            std::string etag;            // verbatim including surrounding quotes
            bool not_modified = false;   // shorthand for status_code == 304
        };

        response get(const request& req);
    };
}
