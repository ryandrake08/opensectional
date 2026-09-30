#pragma once

#include "data_source.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace osect
{
    class elevation_source
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // Opens a terrain tree and reads its manifest.
        explicit elevation_source(std::filesystem::path path, size_t cache_capacity = 128);
        ~elevation_source();

        elevation_source(const elevation_source&) = delete;
        elevation_source& operator=(const elevation_source&) = delete;

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
        std::optional<double> maximum_elevation_ft(double lat_min, double lon_min, double lat_max,
                                                   double lon_max) const;
    };
} // namespace osect
