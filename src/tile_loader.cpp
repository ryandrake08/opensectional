#include "tile_loader.hpp"
#include "program.hpp"
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sdl/log.hpp>
#include <sdl/surface.hpp>
#include <string>
#include <thread>
#include <unordered_set>

namespace osect
{
    struct tile_loader::impl
    {
        struct pending_request
        {
            tile_key key;
            std::filesystem::path path;
        };

        mutable std::mutex mutex;
        std::condition_variable cv;
        std::deque<pending_request> request_queue;
        std::vector<tile_load_result> result_queue;
        std::unordered_set<tile_key> pending_set;
        std::unordered_set<tile_key> failed_set;
        std::thread worker;
        bool shutdown = false;

        void worker_loop()
        {
            while(true)
            {
                auto req = pending_request{};
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this] { return shutdown || !request_queue.empty(); });
                    if(shutdown)
                    {
                        return;
                    }
                    req = std::move(request_queue.front());
                    request_queue.pop_front();
                }

                std::optional<sdl::surface> surf;
                try
                {
                    surf.emplace(req.path.string().c_str());
                }
                catch(const std::exception& e)
                {
                    // Expected at high zoom levels where the tile pyramid
                    // has no data; demoted to debug to keep the warn
                    // channel signal-only.
                    sdl::log_debug(std::string("tile load failed: ") + e.what());
                    std::scoped_lock lock(mutex);
                    pending_set.erase(req.key);
                    failed_set.insert(req.key);
                    continue;
                }

                {
                    std::scoped_lock lock(mutex);
                    result_queue.push_back({req.key, std::move(*surf)});
                }
                wake_main_thread();
            }
        }
    };

    tile_loader::tile_loader() : pimpl(std::make_unique<impl>())
    {
        pimpl->worker = std::thread(&impl::worker_loop, pimpl.get());
    }

    tile_loader::~tile_loader()
    {
        {
            std::scoped_lock lock(pimpl->mutex);
            pimpl->shutdown = true;
        }
        pimpl->cv.notify_one();
        pimpl->worker.join();
    }

    void tile_loader::request(const tile_key& key, const std::filesystem::path& path)
    {
        std::scoped_lock lock(pimpl->mutex);
        if(!pimpl->pending_set.count(key) && !pimpl->failed_set.count(key))
        {
            pimpl->pending_set.insert(key);
            pimpl->request_queue.push_back({key, path});
            pimpl->cv.notify_one();
        }
    }

    bool tile_loader::is_failed(const tile_key& key) const
    {
        std::scoped_lock lock(pimpl->mutex);
        return pimpl->failed_set.count(key) > 0;
    }

    void tile_loader::cancel()
    {
        std::scoped_lock lock(pimpl->mutex);
        for(const auto& req : pimpl->request_queue)
        {
            pimpl->pending_set.erase(req.key);
        }
        pimpl->request_queue.clear();
    }

    std::vector<tile_load_result> tile_loader::drain_results()
    {
        std::scoped_lock lock(pimpl->mutex);
        for(const auto& r : pimpl->result_queue)
        {
            pimpl->pending_set.erase(r.key);
        }
        std::vector<tile_load_result> results;
        results.swap(pimpl->result_queue);
        return results;
    }

} // namespace osect
