#include "tfr_source.hpp"
#include "ephemeral_database.hpp"
#include "http_client.hpp"
#include "program.hpp"
#include "xnotam_parser.hpp"
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sdl/log.hpp>
#include <shared_mutex>
#include <sstream>
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

        std::vector<std::vector<airspace_point>> subdivide_ring(const std::vector<airspace_point>& points)
        {
            std::vector<std::vector<airspace_point>> chunks;
            if(points.empty())
            {
                return chunks;
            }

            std::vector<airspace_point> closed = points;
            if(closed.front().lat != closed.back().lat || closed.front().lon != closed.back().lon)
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
                std::vector<airspace_point> chunk(closed.begin() + static_cast<std::ptrdiff_t>(offset),
                                                  closed.begin() + static_cast<std::ptrdiff_t>(end));
                // chunk is non-empty (end > offset by construction). Read
                // the last element via closed[end-1] rather than chunk.back()
                // to avoid a GCC 15 -Warray-bounds false positive when this
                // function is inlined into append_segments_for.
                const auto& chunk_last = closed[end - 1];
                if(end == n && (chunk_last.lat != closed.front().lat || chunk_last.lon != closed.front().lon))
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
        void append_segments_for(const tfr& t, std::vector<tfr_segment>& out)
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
        // flat objects. We only consume one field per record
        // (`notam_id`), so a full JSON parser is overkill — when a
        // future ephemeral source needs richer parsing, this is the
        // spot to switch to a vendored JSON library. Walk the document
        // once and pull every quoted "notam_id" value's string. Skips
        // escaped quotes inside strings so the scanner doesn't get
        // confused.
        std::vector<std::string> extract_string_field(std::string_view json, std::string_view field)
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
                if(found == std::string_view::npos)
                {
                    break;
                }
                pos = found + needle.size();
                // Skip whitespace and the colon between field and
                // value.
                while(pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':' ||
                                            json[pos] == '\n' || json[pos] == '\r'))
                {
                    ++pos;
                }
                if(pos >= json.size() || json[pos] != '"')
                {
                    continue;
                }
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
                if(pos < json.size())
                {
                    ++pos; // closing quote
                }
            }
            return out;
        }

    }

    // ----------------- impl -----------------

    constexpr auto TFR_REFRESH_PERIOD = std::chrono::minutes(15);
    constexpr auto TFR_EXPIRE_PERIOD = std::chrono::hours(24);

    struct tfr_source::impl
    {
        http_client& http;
        ephemeral_database& db;

        mutable std::shared_mutex mtx;
        std::vector<tfr> tfrs;
        std::vector<tfr_segment> segments;
        std::optional<std::chrono::system_clock::time_point> last_ok;

        // True while refresh() is executing. Read by as_data_source()
        // to surface the UPD tag in the data-status panel.
        std::atomic<bool> refresh_in_progress{false};

        // Background-refresh plumbing. The worker thread sleeps on
        // `cv` until either the 15-min interval expires or shutdown
        // is requested.
        std::mutex worker_mtx;
        std::condition_variable cv;
        bool shutdown = false;
        std::thread worker;

        impl(http_client& h, ephemeral_database& d) : http(h), db(d)
        {
        }
    };

    tfr_source::tfr_source(http_client& http, ephemeral_database& db) : pimpl(std::make_unique<impl>(http, db))
    {
        // Load whatever's in the ephemeral database so warm starts
        // (and --offline runs) have data without network. An empty
        // database leaves the in-memory store empty.
        auto loaded = db.load_tfrs();
        auto last_ok = db.last_refreshed("tfr");

        std::vector<tfr_segment> segs;
        for(const auto& t : loaded)
        {
            append_segments_for(t, segs);
        }

        const auto loaded_count = loaded.size();
        {
            std::unique_lock lock(pimpl->mtx);
            pimpl->tfrs = std::move(loaded);
            pimpl->segments = std::move(segs);
            pimpl->last_ok = last_ok;
        }
        if(loaded_count > 0)
        {
            sdl::log_info("tfr cache loaded: " + std::to_string(loaded_count) + " TFRs");
        }

        // Spin up the refresh worker. Each iteration refreshes if the
        // in-memory data is older than TFR_REFRESH_PERIOD (or absent),
        // then sleeps for TFR_REFRESH_PERIOD or until shutdown. The age
        // check skips a redundant network fetch on a warm start whose
        // cache was written within the last interval.
        pimpl->worker = std::thread(
            [p = pimpl.get(), this]()
            {
                while(true)
                {
                    bool fresh = false;
                    {
                        std::shared_lock lock(p->mtx);
                        if(p->last_ok &&
                           std::chrono::system_clock::now() - *p->last_ok < TFR_REFRESH_PERIOD)
                        {
                            fresh = true;
                        }
                    }
                    if(!fresh)
                    {
                        refresh();
                    }
                    std::unique_lock lk(p->worker_mtx);
                    if(p->cv.wait_for(lk, TFR_REFRESH_PERIOD, [p] { return p->shutdown; }))
                    {
                        return;
                    }
                }
            });
    }

    tfr_source::~tfr_source()
    {
        // Abort any in-flight curl call first so the worker isn't
        // stuck inside curl_easy_perform() when we go to join().
        pimpl->http.cancel();
        {
            std::lock_guard lk(pimpl->worker_mtx);
            pimpl->shutdown = true;
            pimpl->cv.notify_all();
        }
        if(pimpl->worker.joinable())
        {
            pimpl->worker.join();
        }
    }

    void tfr_source::refresh()
    {
        pimpl->refresh_in_progress.store(true);
        wake_main_thread();
        try
        {
            sdl::log_info("tfr refresh starting");
            // 1. Fetch the active TFR list.
            http_client::request list_req;
            list_req.url = "https://tfr.faa.gov/tfrapi/getTfrList";
            const auto list_resp = pimpl->http.get(list_req);
            if(list_resp.status_code != 200)
            {
                throw std::runtime_error("tfr list returned HTTP " + std::to_string(list_resp.status_code));
            }

            const auto notam_ids = extract_string_field(list_resp.body, "notam_id");

            // 2. Fetch each detail XML and parse it.
            std::vector<tfr> new_tfrs;
            new_tfrs.reserve(notam_ids.size());
            for(const auto& notam_id : notam_ids)
            {
                // tfr.faa.gov URL form replaces "/" with "_".
                std::string url_id = notam_id;
                for(auto& c : url_id)
                {
                    if(c == '/')
                    {
                        c = '_';
                    }
                }
                http_client::request det_req;
                det_req.url = "https://tfr.faa.gov/download/detail_" + url_id + ".xml";
                http_client::response det_resp;
                try
                {
                    det_resp = pimpl->http.get(det_req);
                }
                catch(const std::exception& e)
                {
                    sdl::log_warn("tfr detail fetch failed for " + notam_id + ": " + e.what());
                    continue;
                }
                if(det_resp.status_code != 200)
                {
                    sdl::log_warn("tfr detail returned HTTP " + std::to_string(det_resp.status_code) + " for " +
                                  notam_id);
                    continue;
                }
                try
                {
                    auto parsed = parse_xnotam(det_resp.body);
                    if(!parsed)
                    {
                        // Expected for cancellation entries that carry
                        // no polygon geometry
                        sdl::log_info("tfr detail produced no NOTAM for " + notam_id);
                        continue;
                    }
                    parsed->tfr_id = static_cast<int>(new_tfrs.size() + 1);
                    int next_area_id = 1;
                    for(auto& a : parsed->areas)
                    {
                        a.area_id = next_area_id++;
                    }
                    new_tfrs.push_back(std::move(*parsed));
                }
                catch(const std::exception& e)
                {
                    sdl::log_warn("tfr detail parse failed for " + notam_id + ": " + e.what());
                    continue;
                }
            }

            // 3. Build the rendering segment vector.
            std::vector<tfr_segment> new_segs;
            for(const auto& t : new_tfrs)
            {
                append_segments_for(t, new_segs);
            }

            // 4. Atomic swap into the in-memory store.
            const auto now = std::chrono::system_clock::now();
            auto tfr_count = new_tfrs.size();
            auto seg_count = new_segs.size();
            {
                std::unique_lock lock(pimpl->mtx);
                pimpl->tfrs = std::move(new_tfrs);
                pimpl->segments = std::move(new_segs);
                pimpl->last_ok = now;
            }
            sdl::log_info("tfr refresh complete: " + std::to_string(tfr_count) + " TFRs, " + std::to_string(seg_count) +
                          " segments");

            // 5. Persist to the ephemeral database for warm starts
            //    and --offline.
            try
            {
                pimpl->db.replace_tfrs(pimpl->tfrs);
                pimpl->db.set_source_meta("tfr", now, "");
            }
            catch(const std::exception& e)
            {
                sdl::log_warn(std::string("tfr cache write failed: ") + e.what());
            }

        }
        catch(const std::exception& e)
        {
            // Whole refresh failed — preserve the in-memory store.
            sdl::log_warn(std::string("tfr refresh failed: ") + e.what());
        }
        // Clear the in-progress flag and wake the main loop so the
        // status panel flips back from UPD and poll_advance() observes
        // any swap promptly. Runs on both the success and failure paths.
        pimpl->refresh_in_progress.store(false);
        wake_main_thread();
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

    std::optional<std::chrono::system_clock::time_point> tfr_source::last_updated() const
    {
        std::shared_lock lock(pimpl->mtx);
        return pimpl->last_ok;
    }

    data_source tfr_source::as_data_source() const
    {
        data_source ds;
        ds.name = "tfr";
        const auto last = last_updated();
        if(last)
        {
            ds.expires = *last + TFR_EXPIRE_PERIOD;
            const auto t = std::chrono::system_clock::to_time_t(*last);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            // Pin to the "C" locale so month abbreviations are always
            // English ("Apr", not "avr." / "4月" / etc.), matching the
            // other data sources' info strings.
            std::ostringstream oss;
            oss.imbue(std::locale::classic());
            oss << "TFR " << std::put_time(&tm, "%d %b %Y");
            ds.info = oss.str();
        }
        else
        {
            ds.info = "TFR (no data yet)";
        }
        ds.updating = pimpl->refresh_in_progress.load();
        return ds;
    }
}
