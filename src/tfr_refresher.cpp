#include "tfr_refresher.hpp"
#include "ephemeral_database.hpp"
#include "ephemeral_source.hpp"
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
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace osect
{
    namespace
    {
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

    struct tfr_refresher::impl
    {
        http_client http;
        ephemeral_database db;

        // `last_ok` and `refresh_in_progress` are the only runtime
        // state this class still tracks; the parsed TFR data lives
        // entirely in `db`. A plain mutex protects `last_ok`;
        // `refresh_in_progress` is atomic for lock-free reads from
        // the data-status panel.
        mutable std::mutex last_ok_mtx;
        std::optional<std::chrono::system_clock::time_point> last_ok;
        std::atomic<bool> refresh_in_progress{false};

        // Background-refresh plumbing. The worker thread sleeps on
        // `cv` until either the 15-min interval expires or shutdown
        // is requested.
        std::mutex worker_mtx;
        std::condition_variable cv;
        bool shutdown = false;
        std::thread worker;

        // Seeds last_ok from whatever the database remembers from
        // prior runs so the worker's "is data fresh enough?" check
        // can skip a network fetch on a warm start whose cache was
        // written within the last interval. Member-init order
        // (declaration order) guarantees `db` is constructed before
        // `last_ok` reads from it. No lock needed — no other thread
        // has a reference to this object yet.
        impl(bool offline, const std::filesystem::path& db_path)
            : http(offline), db(db_path), last_ok(db.last_refreshed("tfr"))
        {
        }

        // Pull the active TFR list, fetch each detail XML, write
        // the new state through the owned database connection,
        // update last_ok, and push an ephemeral_refresh event.
        // Catches every libcurl / parse failure so a single outage
        // doesn't leave the app stuck; on failure the database
        // keeps its prior contents.
        void refresh()
        {
            refresh_in_progress.store(true);
            wake_main_thread();
            try
            {
                sdl::log_info("tfr refresh starting");
                // 1. Fetch the active TFR list.
                http_client::request list_req;
                list_req.url = "https://tfr.faa.gov/tfrapi/getTfrList";
                const auto list_resp = http.get(list_req);
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
                        det_resp = http.get(det_req);
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

                // 3. Persist the new state through the database.
                //    The DB is the source of truth — until this
                //    commit lands, consumers see the previous TFR
                //    set.
                const auto now = std::chrono::system_clock::now();
                const auto tfr_count = new_tfrs.size();
                db.replace_tfrs(new_tfrs);
                db.set_source_meta("tfr", now, "");

                // 4. Update last_ok so the worker's freshness check
                //    sees the new timestamp and the data-status panel
                //    reports "current."
                {
                    std::lock_guard lock(last_ok_mtx);
                    last_ok = now;
                }
                sdl::log_info("tfr refresh complete: " + std::to_string(tfr_count) + " TFRs");

                // 5. Notify the main thread that ephemeral TFR data
                //    is now fresh. The push doubles as a wake — the
                //    event arrives through dispatch_events and the
                //    registered handler invalidates the relevant
                //    feature build, so the next build re-queries the
                //    database and sees the new TFRs.
                push_ephemeral_refresh(ephemeral_source::tfr);
            }
            catch(const std::exception& e)
            {
                // Whole refresh failed — the database keeps its
                // prior contents and last_ok stays unchanged, so
                // consumers continue seeing whatever data was last
                // persisted.
                sdl::log_warn(std::string("tfr refresh failed: ") + e.what());
            }
            // Clear the in-progress flag and wake the main loop so
            // the status panel flips back from UPD. The success-path
            // notification of new data already happened via
            // push_ephemeral_refresh above; this wake is for the
            // failure path (no event was pushed) and to ensure the
            // status panel observes the flag transition promptly.
            refresh_in_progress.store(false);
            wake_main_thread();
        }

        // Periodic worker. Refreshes if the on-disk cache is older
        // than TFR_REFRESH_PERIOD (or absent), then sleeps for
        // TFR_REFRESH_PERIOD or until shutdown. Spawned by the
        // outer constructor; matches the impl::worker_loop pattern
        // used by feature_builder and tile_loader.
        void worker_loop()
        {
            while(true)
            {
                bool fresh = false;
                {
                    std::lock_guard lock(last_ok_mtx);
                    if(last_ok && std::chrono::system_clock::now() - *last_ok < TFR_REFRESH_PERIOD)
                    {
                        fresh = true;
                    }
                }
                if(!fresh)
                {
                    refresh();
                }
                std::unique_lock lk(worker_mtx);
                if(cv.wait_for(lk, TFR_REFRESH_PERIOD, [this] { return shutdown; }))
                {
                    return;
                }
            }
        }
    };

    tfr_refresher::tfr_refresher(bool offline, const std::filesystem::path& db_path)
        : pimpl(std::make_unique<impl>(offline, db_path))
    {
        pimpl->worker = std::thread(&impl::worker_loop, pimpl.get());
    }

    tfr_refresher::~tfr_refresher()
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

    void tfr_refresher::refresh()
    {
        pimpl->refresh();
    }

    std::optional<std::chrono::system_clock::time_point> tfr_refresher::last_updated() const
    {
        std::lock_guard lock(pimpl->last_ok_mtx);
        return pimpl->last_ok;
    }

    data_source tfr_refresher::as_data_source() const
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
