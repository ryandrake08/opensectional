#include "http_client.hpp"

#include <cctype>
#include <curl/curl.h>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace osect
{
    namespace
    {
        // libcurl requires curl_global_init() exactly once before any
        // handle is created. The corresponding global cleanup is
        // explicitly documented as risky in mixed-library processes
        // (see CURLOPT_NOSIGNAL and the curl_global_init manpage), so
        // we never call it — process exit handles release.
        std::once_flag global_init_flag;
        void ensure_global_init()
        {
            std::call_once(global_init_flag, []() {
                curl_global_init(CURL_GLOBAL_DEFAULT);
            });
        }

        size_t write_body_cb(char* ptr, size_t size, size_t nmemb,
                             void* userdata)
        {
            const auto total = size * nmemb;
            static_cast<std::string*>(userdata)->append(ptr, total);
            return total;
        }

        // Header callback: scan each line for ETag and capture its
        // value (verbatim, including surrounding quotes — that's the
        // shape callers will hand back as If-None-Match).
        size_t header_cb(char* buffer, size_t size, size_t nitems,
                         void* userdata)
        {
            const auto total = size * nitems;
            std::string_view line(buffer, total);
            // Strip CRLF.
            while(!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.remove_suffix(1);
            constexpr std::string_view prefix("ETag:");
            if(line.size() < prefix.size()) return total;
            for(std::size_t i = 0; i < prefix.size(); ++i)
            {
                if(std::tolower(static_cast<unsigned char>(line[i])) !=
                   std::tolower(static_cast<unsigned char>(prefix[i])))
                    return total;
            }
            line.remove_prefix(prefix.size());
            while(!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.remove_prefix(1);
            static_cast<std::string*>(userdata)->assign(line);
            return total;
        }
    }

    struct http_client::impl
    {
        CURL* curl = nullptr;
        bool offline = false;

        explicit impl(bool offline_) : offline(offline_)
        {
            ensure_global_init();
            curl = curl_easy_init();
            if(!curl) throw std::runtime_error("curl_easy_init failed");
        }
        ~impl()
        {
            if(curl) curl_easy_cleanup(curl);
        }
        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
    };

    http_client::http_client(bool offline) : pimpl(std::make_unique<impl>(offline)) {}
    http_client::~http_client() = default;

    http_client::response http_client::get(const request& req)
    {
        if(pimpl->offline)
        {
            throw std::runtime_error(
                "HTTP request failed: offline mode (" + req.url + ")");
        }

        auto* curl = pimpl->curl;
        curl_easy_reset(curl);

        response resp;
        struct curl_slist* headers = nullptr;
        std::string if_none_match_header;
        if(!req.if_none_match.empty())
        {
            if_none_match_header = "If-None-Match: " + req.if_none_match;
            headers = curl_slist_append(headers,
                                         if_none_match_header.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp.etag);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(req.timeout.count()));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, req.user_agent.c_str());
        // Empty string means "any built-in encoding" — curl negotiates
        // gzip / deflate transparently and decompresses the body.
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        // We're embedding curl into a GUI app where SIGPIPE handling is
        // already centralized; keep curl from installing its own.
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        if(headers)
        {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        const auto rc = curl_easy_perform(curl);
        if(headers)
        {
            curl_slist_free_all(headers);
        }

        if(rc != CURLE_OK)
        {
            throw std::runtime_error(
                std::string("HTTP request failed: ") +
                curl_easy_strerror(rc) + " (" + req.url + ")");
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        resp.status_code = static_cast<int>(http_code);
        resp.not_modified = http_code == 304;
        return resp;
    }
}
