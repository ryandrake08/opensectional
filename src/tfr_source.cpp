#include "tfr_source.hpp"

#include "ephemeral_cache.hpp"
#include "http_client.hpp"
#include "wake_main.hpp"
#include "xnotam_parser.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace osect
{
    namespace
    {
        // ---- subdivide_ring ----
        //
        // Mirror of tools/build_common.py's subdivide_ring. Each chunk
        // has at most max_points points and overlaps the next by 1.
        // The final chunk wraps back to include the first point so
        // the ring renders closed. Returns a list of point lists.
        constexpr std::size_t MAX_POINTS = 32;

        std::vector<std::vector<airspace_point>>
        subdivide_ring(const std::vector<airspace_point>& points)
        {
            std::vector<std::vector<airspace_point>> chunks;
            if(points.empty()) return chunks;

            std::vector<airspace_point> closed = points;
            if(closed.front().lat != closed.back().lat ||
               closed.front().lon != closed.back().lon)
            {
                closed.push_back(closed.front());
            }
            const std::size_t n = closed.size();

            if(n <= MAX_POINTS)
            {
                chunks.push_back(std::move(closed));
                return chunks;
            }

            const std::size_t stride = MAX_POINTS - 1;
            std::size_t offset = 0;
            while(offset < n - 1)
            {
                const std::size_t end = std::min(offset + MAX_POINTS, n);
                std::vector<airspace_point> chunk(
                    closed.begin() + static_cast<std::ptrdiff_t>(offset),
                    closed.begin() + static_cast<std::ptrdiff_t>(end));
                // chunk is non-empty (end > offset by construction). Read
                // the last element via closed[end-1] rather than chunk.back()
                // to avoid a GCC 15 -Warray-bounds false positive when this
                // function is inlined into append_segments_for.
                const auto& chunk_last = closed[end - 1];
                if(end == n &&
                   (chunk_last.lat != closed.front().lat ||
                    chunk_last.lon != closed.front().lon))
                {
                    chunk.push_back(closed.front());
                }
                chunks.push_back(std::move(chunk));
                offset += stride;
            }
            return chunks;
        }

        // Turn a parsed `tfr` into the `tfr_segment` vector the
        // renderer expects — one segment per chunk per area, carrying
        // the area's altitude band so the render path can style by it.
        void append_segments_for(const tfr& t,
                                  std::vector<tfr_segment>& out)
        {
            for(const auto& a : t.areas)
            {
                for(auto& chunk : subdivide_ring(a.points))
                {
                    tfr_segment seg{};
                    seg.upper_ft_val = a.upper_ft_val;
                    seg.upper_ft_ref = a.upper_ft_ref;
                    seg.lower_ft_val = a.lower_ft_val;
                    seg.lower_ft_ref = a.lower_ft_ref;
                    seg.points = std::move(chunk);
                    out.push_back(std::move(seg));
                }
            }
        }

        // ---- minimal JSON-array string extractor ----
        //
        // The tfr.faa.gov list endpoint returns a top-level array of
        // objects. We only consume one field per record (`notam_id`),
        // so a full JSON parser is overkill — when a future ephemeral
        // source needs richer parsing, this is the spot to switch to
        // a vendored JSON library. Walk the document once and pull
        // every quoted "notam_id" value's string. Skips escaped
        // quotes inside strings so the scanner doesn't get confused.
        std::vector<std::string>
        extract_string_field(std::string_view json, std::string_view field)
        {
            std::vector<std::string> out;
            // Pattern we look for: "<field>" : "..."
            std::string needle;
            needle.reserve(field.size() + 2);
            needle.push_back('"');
            needle.append(field);
            needle.push_back('"');

            std::size_t pos = 0;
            while(true)
            {
                const auto found = json.find(needle, pos);
                if(found == std::string_view::npos) break;
                pos = found + needle.size();
                // Skip whitespace and the colon between field and
                // value.
                while(pos < json.size() &&
                      (json[pos] == ' ' || json[pos] == '\t' ||
                       json[pos] == ':' || json[pos] == '\n' ||
                       json[pos] == '\r')) ++pos;
                if(pos >= json.size() || json[pos] != '"') continue;
                ++pos;
                std::string value;
                while(pos < json.size() && json[pos] != '"')
                {
                    if(json[pos] == '\\' && pos + 1 < json.size())
                    {
                        // Pass-through for the few escapes that show
                        // up in FAA notam_ids (none in practice; the
                        // field is plain ASCII like "6/4045").
                        value.push_back(json[pos + 1]);
                        pos += 2;
                        continue;
                    }
                    value.push_back(json[pos]);
                    ++pos;
                }
                out.push_back(std::move(value));
                if(pos < json.size()) ++pos;  // closing quote
            }
            return out;
        }

        // ---- cache serialization ----
        //
        // tfr_source stores the parsed in-memory state (vector<tfr>)
        // as a single binary blob under cache key "tfr". This is a
        // post-parse cache: the unparsed transport state for TFR is
        // ~100 separate XML responses, which doesn't fit
        // ephemeral_cache's one-blob-per-key contract without an
        // envelope. Storing the parsed form keeps the cache layer
        // generic and lets warm starts skip both the network and the
        // re-parse.
        constexpr std::array<char, 4> CACHE_MAGIC = {'T', 'F', 'R', 'C'};
        constexpr std::uint32_t CACHE_VERSION = 1;

        template<typename T>
        void put_pod(std::string& out, T value)
        {
            out.append(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        template<typename T>
        bool get_pod(std::string_view& cur, T& value)
        {
            if(cur.size() < sizeof(T)) return false;
            std::memcpy(&value, cur.data(), sizeof(T));
            cur.remove_prefix(sizeof(T));
            return true;
        }

        void put_string(std::string& out, const std::string& s)
        {
            put_pod(out, static_cast<std::uint32_t>(s.size()));
            out.append(s);
        }

        bool get_string(std::string_view& cur, std::string& out)
        {
            std::uint32_t len = 0;
            if(!get_pod(cur, len)) return false;
            if(cur.size() < len) return false;
            out.assign(cur.data(), len);
            cur.remove_prefix(len);
            return true;
        }

        std::string serialize(const std::vector<tfr>& tfrs)
        {
            std::string out;
            out.append(CACHE_MAGIC.data(), CACHE_MAGIC.size());
            put_pod(out, CACHE_VERSION);
            put_pod(out, static_cast<std::uint32_t>(tfrs.size()));
            for(const auto& t : tfrs)
            {
                put_pod(out, static_cast<std::uint32_t>(t.tfr_id));
                put_string(out, t.notam_id);
                put_string(out, t.tfr_type);
                put_string(out, t.facility);
                put_string(out, t.date_effective);
                put_string(out, t.date_expire);
                put_string(out, t.description);
                put_pod(out, static_cast<std::uint32_t>(t.areas.size()));
                for(const auto& a : t.areas)
                {
                    put_pod(out, static_cast<std::uint32_t>(a.area_id));
                    put_string(out, a.area_name);
                    put_pod(out, static_cast<std::int32_t>(a.upper_ft_val));
                    put_string(out, a.upper_ft_ref);
                    put_pod(out, static_cast<std::int32_t>(a.lower_ft_val));
                    put_string(out, a.lower_ft_ref);
                    put_string(out, a.date_effective);
                    put_string(out, a.date_expire);
                    put_pod(out, static_cast<std::uint32_t>(a.points.size()));
                    for(const auto& p : a.points)
                    {
                        put_pod(out, p.lat);
                        put_pod(out, p.lon);
                    }
                }
            }
            return out;
        }

        bool deserialize(const std::string& body,
                         std::vector<tfr>& out)
        {
            std::string_view cur(body);
            if(cur.size() < CACHE_MAGIC.size()) return false;
            if(std::memcmp(cur.data(), CACHE_MAGIC.data(),
                           CACHE_MAGIC.size()) != 0) return false;
            cur.remove_prefix(CACHE_MAGIC.size());

            std::uint32_t version = 0;
            if(!get_pod(cur, version) || version != CACHE_VERSION)
                return false;

            std::uint32_t tfr_count = 0;
            if(!get_pod(cur, tfr_count)) return false;
            if(tfr_count > 100000) return false;  // sanity cap

            out.reserve(tfr_count);
            for(std::uint32_t i = 0; i < tfr_count; ++i)
            {
                tfr t{};
                std::uint32_t id = 0;
                if(!get_pod(cur, id)) return false;
                t.tfr_id = static_cast<int>(id);
                if(!get_string(cur, t.notam_id) ||
                   !get_string(cur, t.tfr_type) ||
                   !get_string(cur, t.facility) ||
                   !get_string(cur, t.date_effective) ||
                   !get_string(cur, t.date_expire) ||
                   !get_string(cur, t.description)) return false;
                std::uint32_t area_count = 0;
                if(!get_pod(cur, area_count)) return false;
                if(area_count > 10000) return false;
                t.areas.reserve(area_count);
                for(std::uint32_t j = 0; j < area_count; ++j)
                {
                    tfr_area a{};
                    std::uint32_t aid = 0;
                    if(!get_pod(cur, aid)) return false;
                    a.area_id = static_cast<int>(aid);
                    if(!get_string(cur, a.area_name)) return false;
                    std::int32_t up = 0;
                    if(!get_pod(cur, up)) return false;
                    a.upper_ft_val = up;
                    if(!get_string(cur, a.upper_ft_ref)) return false;
                    std::int32_t lo = 0;
                    if(!get_pod(cur, lo)) return false;
                    a.lower_ft_val = lo;
                    if(!get_string(cur, a.lower_ft_ref) ||
                       !get_string(cur, a.date_effective) ||
                       !get_string(cur, a.date_expire)) return false;
                    std::uint32_t pt_count = 0;
                    if(!get_pod(cur, pt_count)) return false;
                    if(pt_count > 1000000) return false;
                    a.points.reserve(pt_count);
                    for(std::uint32_t k = 0; k < pt_count; ++k)
                    {
                        airspace_point p{};
                        if(!get_pod(cur, p.lat) || !get_pod(cur, p.lon))
                            return false;
                        a.points.push_back(p);
                    }
                    t.areas.push_back(std::move(a));
                }
                out.push_back(std::move(t));
            }
            return true;
        }
    }

    // Free function so tfr_source's tests can build a cache body
    // without depending on the full live-fetch path. Kept in the
    // anonymous namespace for non-test users; exposed via this
    // alias so the test TU can link against it.
    std::string tfr_source_serialize_for_test(const std::vector<tfr>& v)
    {
        return serialize(v);
    }

    // ----------------- impl -----------------

    constexpr auto REFRESH_INTERVAL = std::chrono::minutes(15);
    constexpr auto STALENESS_HORIZON = std::chrono::hours(24);

    struct tfr_source::impl
    {
        http_client& http;
        ephemeral_cache& cache;

        mutable std::shared_mutex mtx;
        std::vector<tfr> tfrs;
        std::vector<tfr_segment> segments;
        std::optional<std::chrono::system_clock::time_point> last_ok;

        // Background-refresh plumbing. The worker thread sleeps on
        // `cv` until either the 15-min interval expires or shutdown
        // is requested.
        std::mutex worker_mtx;
        std::condition_variable cv;
        bool shutdown = false;
        std::thread worker;

        impl(http_client& h, ephemeral_cache& c) : http(h), cache(c) {}
    };

    tfr_source::tfr_source(http_client& http, ephemeral_cache& cache)
        : pimpl(std::make_unique<impl>(http, cache))
    {
        // Load whatever's in the cache so warm starts (and
        // --offline runs) have data without network. Cache misses
        // and corrupt files leave the in-memory store empty.
        if(const auto entry = cache.load("tfr"))
        {
            std::vector<tfr> loaded;
            if(deserialize(entry->body, loaded))
            {
                std::vector<tfr_segment> segs;
                for(const auto& t : loaded) append_segments_for(t, segs);

                // Best-effort wall-clock time for the cached blob —
                // file_time_type and system_clock are different
                // clocks before C++20, so we approximate via the
                // current delta. Off by at most a few seconds.
                std::optional<std::chrono::system_clock::time_point> mtime;
                std::error_code ec;
                const auto path = cache.dir() / "tfr.bin";
                const auto ftime =
                    std::filesystem::last_write_time(path, ec);
                if(!ec)
                {
                    const auto fs_now =
                        std::filesystem::file_time_type::clock::now();
                    mtime = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<
                            std::chrono::system_clock::duration>(
                                ftime - fs_now);
                }

                std::unique_lock lock(pimpl->mtx);
                pimpl->tfrs = std::move(loaded);
                pimpl->segments = std::move(segs);
                pimpl->last_ok = mtime;
            }
        }

        // Spin up the refresh worker. Refresh once immediately so a
        // freshly-launched app with no cache pulls data right away,
        // then loop on the 15-min interval until shutdown.
        pimpl->worker = std::thread([p = pimpl.get(), this]() {
            while(true)
            {
                refresh();
                std::unique_lock lk(p->worker_mtx);
                if(p->cv.wait_for(lk, REFRESH_INTERVAL,
                    [p]{ return p->shutdown; }))
                    return;
            }
        });
    }

    tfr_source::~tfr_source()
    {
        {
            std::lock_guard lk(pimpl->worker_mtx);
            pimpl->shutdown = true;
            pimpl->cv.notify_all();
        }
        if(pimpl->worker.joinable()) pimpl->worker.join();
    }

    void tfr_source::refresh()
    {
        try
        {
            // 1. Fetch the active TFR list.
            http_client::request list_req;
            list_req.url = "https://tfr.faa.gov/tfrapi/getTfrList";
            const auto list_resp = pimpl->http.get(list_req);
            if(list_resp.status_code != 200)
            {
                throw std::runtime_error(
                    "tfr list returned HTTP " +
                    std::to_string(list_resp.status_code));
            }

            const auto notam_ids =
                extract_string_field(list_resp.body, "notam_id");

            // 2. Fetch each detail XML and parse it.
            std::vector<tfr> new_tfrs;
            new_tfrs.reserve(notam_ids.size());
            for(const auto& notam_id : notam_ids)
            {
                // tfr.faa.gov URL form replaces "/" with "_".
                std::string url_id = notam_id;
                for(auto& c : url_id) if(c == '/') c = '_';
                http_client::request det_req;
                det_req.url =
                    "https://tfr.faa.gov/download/detail_" + url_id + ".xml";
                http_client::response det_resp;
                try
                {
                    det_resp = pimpl->http.get(det_req);
                }
                catch(const std::exception& e)
                {
                    std::cerr << "tfr detail fetch failed for "
                              << notam_id << ": " << e.what() << "\n";
                    continue;
                }
                if(det_resp.status_code != 200) continue;
                try
                {
                    auto parsed = parse_xnotam(det_resp.body);
                    if(!parsed) continue;
                    parsed->tfr_id = static_cast<int>(new_tfrs.size() + 1);
                    int next_area_id = 1;
                    for(auto& a : parsed->areas)
                        a.area_id = next_area_id++;
                    new_tfrs.push_back(std::move(*parsed));
                }
                catch(const std::exception& e)
                {
                    std::cerr << "tfr detail parse failed for "
                              << notam_id << ": " << e.what() << "\n";
                    continue;
                }
            }

            // 3. Build the rendering segment vector.
            std::vector<tfr_segment> new_segs;
            for(const auto& t : new_tfrs) append_segments_for(t, new_segs);

            // 4. Atomic swap into the in-memory store.
            {
                std::unique_lock lock(pimpl->mtx);
                pimpl->tfrs = std::move(new_tfrs);
                pimpl->segments = std::move(new_segs);
                pimpl->last_ok = std::chrono::system_clock::now();
            }

            // 5. Persist to disk for warm starts and --offline.
            try
            {
                pimpl->cache.store("tfr", serialize(pimpl->tfrs), "");
            }
            catch(const std::exception& e)
            {
                std::cerr << "tfr cache write failed: " << e.what() << "\n";
            }

            // 6. Wake the main loop so poll_advance() observes the swap
            //    promptly instead of waiting for the next input event.
            wake_main_thread();
        }
        catch(const std::exception& e)
        {
            // Whole refresh failed — preserve the in-memory store.
            std::cerr << "tfr refresh failed: " << e.what() << "\n";
        }
    }

    std::vector<tfr> tfr_source::snapshot() const
    {
        std::shared_lock lock(pimpl->mtx);
        return pimpl->tfrs;
    }

    std::vector<tfr_segment> tfr_source::snapshot_segments() const
    {
        std::shared_lock lock(pimpl->mtx);
        return pimpl->segments;
    }

    std::optional<std::chrono::system_clock::time_point>
    tfr_source::last_updated() const
    {
        std::shared_lock lock(pimpl->mtx);
        return pimpl->last_ok;
    }

    data_source tfr_source::as_data_source() const
    {
        data_source ds;
        ds.name = "tfr";
        const auto last = last_updated();
        if(last) ds.expires = *last + STALENESS_HORIZON;
        ds.info = last ? "TFR (in-memory)" : "TFR (no data yet)";
        return ds;
    }
}
