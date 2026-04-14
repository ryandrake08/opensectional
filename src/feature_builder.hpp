#pragma once

#include "line_renderer.hpp"
#include "pick_result.hpp"
#include "ui_overlay.hpp"
#include <array>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nasrbrowse
{
    class chart_style;

    // Geometry data for a single feature layer
    struct polyline_data
    {
        std::vector<std::vector<glm::vec2>> polylines;
        std::vector<line_style> styles;
        std::vector<circle_data> circles;

        void clear()
        {
            polylines.clear();
            styles.clear();
            circles.clear();
        }
    };

    struct polygon_fill_vertex
    {
        glm::vec2 pos;    // world-space Mercator
        glm::vec4 color;  // RGBA in [0,1]
    };

    // Pre-triangulated polygon fills: flat triangle list, 3 vertices per
    // triangle, all polygons concatenated.
    struct polygon_fill_data
    {
        std::vector<polygon_fill_vertex> triangles;
        void clear() { triangles.clear(); }
    };

    // Parameters the worker needs to build feature geometry
    struct feature_build_request
    {
        double lon_min, lat_min, lon_max, lat_max;
        double half_extent_y;
        int viewport_height;
        double zoom;
        altitude_filter altitude;
        std::optional<pick_feature> selection;
    };

    // A label candidate from the feature build pass (world-space)
    struct label_candidate
    {
        std::string text;
        double mx, my;              // World-space Mercator meters
        int priority;               // Higher = placed first
        int layer;                  // layer_id for visibility filtering
    };

    // Completed feature geometry from the worker
    struct feature_build_result
    {
        std::array<polyline_data, layer_sdf_count> poly;
        std::vector<label_candidate> labels;

        // Primitives for the currently-selected feature (halo + re-emitted
        // icon). Rendered after all layers so the selection sits on top.
        polyline_data selection_overlay;
        polygon_fill_data selection_fill;
    };

    // Background worker that builds feature polylines from the NASR database.
    // Owns its own database connection and worker thread.
    class feature_builder
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        feature_builder(const char* db_path, const chart_style& cs);
        ~feature_builder();

        feature_builder(const feature_builder&) = delete;
        feature_builder& operator=(const feature_builder&) = delete;

        // Submit a build request. Replaces any pending (not yet started) request.
        void request(const feature_build_request& req);

        // Return the completed result, if any.
        std::optional<feature_build_result> drain_result();
    };

} // namespace nasrbrowse
