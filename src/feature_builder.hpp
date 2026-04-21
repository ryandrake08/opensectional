#pragma once

#include "flight_route.hpp"
#include "line_renderer.hpp"
#include "pick_result.hpp"
#include "ui_overlay.hpp"
#include <array>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace nasrbrowse
{
    class chart_style;
    class nasr_database;

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
        std::optional<feature> selection;
        std::optional<flight_route> route;
        bool route_selected = true;
    };

    // A label candidate from the feature build pass (world-space)
    struct label_candidate
    {
        std::string text;
        double mx, my;              // World-space Mercator meters
        int priority;               // Higher = placed first
        int layer;                  // layer_id for visibility filtering
        float angle = 0.0F;         // Rotation in radians (0 = horizontal above point)
        std::string upper_text = {};  // Composite airspace label (when non-empty)
        std::string lower_text = {};
        uint8_t outline_r = 0, outline_g = 0, outline_b = 0;
    };

    // Cross-feature scratch state shared across a single build pass.
    // `navaid_positions` is populated by the navaid builder and consumed
    // by the airway builder (for pull-away-from-navaid clearance).
    // `airway_waypoints` is populated by the airway builder and consumed
    // by the fix builder (for the on-airway visibility test).
    struct feature_build_state
    {
        std::vector<glm::vec2> navaid_positions;
        float navaid_clearance = 0;
        std::unordered_set<std::string> airway_waypoints;
    };

    // Inputs bundled for feature_type::build(). One instance is built per
    // antimeridian copy by feature_builder and passed to every feature_type.
    struct build_context
    {
        const nasr_database& db;
        const chart_style& styles;
        const feature_build_request& req;
        double mx_offset;
        std::array<polyline_data, layer_sdf_count>& poly;
        std::vector<label_candidate>& labels;
        feature_build_state& state;

        // Test whether (x, y) is at (or very close to) a previously-rendered
        // navaid; used by the airway builder to pull airway endpoints away
        // from navaid icons so the navaid graphic stays readable.
        bool is_at_navaid(float x, float y) const;

        // True if the fix with the given id is an endpoint of some rendered
        // airway segment — drives the "fix_airway" vs "fix_noairway"
        // visibility bucket.
        bool fix_on_airway(const std::string& fix_id) const;
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
