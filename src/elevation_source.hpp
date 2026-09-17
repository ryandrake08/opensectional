#pragma once

#include "data_source.hpp"
#include "elevation_tile.hpp"
#include "tile_key.hpp"

#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace osect
{
    class elevation_source
    {
        std::filesystem::path path_;
        bool available_ = false;
        int min_zoom_ = 0;
        int max_zoom_ = 0;
        int tile_size_ = 0;
        int skirt_ = 0;
        bool has_water_mask_ = false;
        int water_min_zoom_ = 0;
        int water_max_zoom_ = 0;
        double vertical_precision_m_ = 0.0;
        std::string attribution_;
        std::string display_name_;
        std::string status_name_;
        std::string source_version_;
        bool surface_model_ = false;
        size_t cache_capacity_;

        struct cache_entry
        {
            std::shared_ptr<const elevation_tile> tile;
            std::list<tile_key>::iterator recency;
        };
        mutable std::mutex cache_mutex_;
        mutable std::unordered_map<tile_key, cache_entry> cache_;
        mutable std::list<tile_key> recency_;

        // Loads a tile on demand and retains it in the LRU cache.
        std::shared_ptr<const elevation_tile> load_tile(const tile_key& key) const;
        // Promotes a cache entry to most recently used.
        void touch(std::unordered_map<tile_key, cache_entry>::iterator entry) const;

    public:
        // Opens a terrain tree and reads its manifest.
        explicit elevation_source(std::filesystem::path path, size_t cache_capacity = 128);

        bool available() const;
        int min_zoom() const;
        int max_zoom() const;
        int tile_size() const;
        int skirt() const;
        // The water/ sidecar tree (recorded in manifest.json). Built
        // over the same zoom range as the height tiles.
        bool has_water_mask() const;
        int water_min_zoom() const;
        int water_max_zoom() const;
        double vertical_precision_m() const;
        const std::filesystem::path& path() const;
        const std::string& attribution() const;
        bool is_surface_model() const;
        // Returns the source metadata used by the data-status panel.
        data_source data_source_row() const;
        // Samples one terrain tile and converts the result to feet.
        std::optional<double> elevation_ft(double lat, double lon, int zoom) const;
        // Returns the conservative maximum terrain elevation in a geographic box.
        std::optional<double> maximum_elevation_ft(double lat_min, double lon_min, double lat_max, double lon_max) const;
    };
} // namespace osect
