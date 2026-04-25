#pragma once

#include "tile_key.hpp"
#include <memory>
#include <string>
#include <vector>

namespace sdl
{
    class surface;
}

namespace osect
{

    struct tile_load_result
    {
        tile_key key;
        std::unique_ptr<sdl::surface> surf;
    };

    // Background tile loader thread.
    // Loads PNG files from disk on a worker thread, returning
    // sdl::surface objects ready for GPU upload on the main thread.
    class tile_loader
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        tile_loader();
        ~tile_loader();

        tile_loader(const tile_loader&) = delete;
        tile_loader& operator=(const tile_loader&) = delete;

        // Enqueue a tile for background loading.
        // Pushes a user event when loading completes to wake the main loop.
        void request(const tile_key& key, const std::string& path);

        // Clear all queued requests (in-flight and completed are unaffected)
        void cancel();

        // Check if a tile has previously failed to load
        bool is_failed(const tile_key& key) const;

        // Drain completed results (call from main thread)
        std::vector<tile_load_result> drain_results();
    };

} // namespace osect
