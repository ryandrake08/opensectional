#pragma once

#include "tile_key.hpp"
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <sdl/log.hpp>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace osect
{
    // Loads tiles on one background thread. load runs on that thread and
    // throws when there is no tile for the key; the key is then marked
    // failed and is never loaded again. on_ready runs on that thread after
    // every load, successful or not, so the main loop can wake and drain.
    template <typename result_t>
    class tile_loader
    {
        std::function<result_t(const tile_key&)> load_;
        std::function<void()> on_ready_;

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<tile_key> requests_;
        std::vector<std::pair<tile_key, result_t>> results_;
        // Requested, loading, or loaded but not yet drained.
        std::unordered_set<tile_key> pending_;
        std::unordered_set<tile_key> failed_;
        bool shutdown_ = false;

        // Declared last so the members above exist before the thread starts.
        std::thread worker_;

        void run()
        {
            while(true)
            {
                tile_key key;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return shutdown_ || !requests_.empty(); });
                    if(shutdown_)
                    {
                        return;
                    }
                    key = requests_.front();
                    requests_.pop_front();
                }

                try
                {
                    result_t result = load_(key);
                    std::scoped_lock lock(mutex_);
                    results_.emplace_back(key, std::move(result));
                }
                catch(const std::exception& e)
                {
                    // Expected where the tile pyramid has no data; debug
                    // level keeps the warn channel signal-only.
                    sdl::log_debug(std::string("tile load failed: ") + e.what());
                    std::scoped_lock lock(mutex_);
                    pending_.erase(key);
                    failed_.insert(key);
                }
                on_ready_();
            }
        }

    public:
        tile_loader(std::function<result_t(const tile_key&)> load, std::function<void()> on_ready)
            : load_(std::move(load)), on_ready_(std::move(on_ready)), worker_(&tile_loader::run, this)
        {
        }

        ~tile_loader()
        {
            {
                std::scoped_lock lock(mutex_);
                shutdown_ = true;
            }
            cv_.notify_one();
            worker_.join();
        }

        tile_loader(const tile_loader&) = delete;
        tile_loader& operator=(const tile_loader&) = delete;
        tile_loader(tile_loader&&) = delete;
        tile_loader& operator=(tile_loader&&) = delete;

        // Queues key for loading unless it is already pending or has failed.
        void request(const tile_key& key)
        {
            std::scoped_lock lock(mutex_);
            if(pending_.count(key) == 0 && failed_.count(key) == 0)
            {
                pending_.insert(key);
                requests_.push_back(key);
                cv_.notify_one();
            }
        }

        // Drops queued requests that have not started loading.
        void cancel()
        {
            std::scoped_lock lock(mutex_);
            for(const auto& key : requests_)
            {
                pending_.erase(key);
            }
            requests_.clear();
        }

        bool failed(const tile_key& key) const
        {
            std::scoped_lock lock(mutex_);
            return failed_.count(key) != 0;
        }

        // Takes the results loaded since the last call. Call from the main thread.
        std::vector<result_t> drain()
        {
            std::vector<result_t> drained;
            std::scoped_lock lock(mutex_);
            drained.reserve(results_.size());
            for(auto& loaded : results_)
            {
                pending_.erase(loaded.first);
                drained.push_back(std::move(loaded.second));
            }
            results_.clear();
            return drained;
        }
    };
} // namespace osect
