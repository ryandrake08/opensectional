#pragma once

#include "feature_builder.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace sdl
{
    class device;
    class copy_pass;
    class render_pass;
}

namespace osect
{
    struct render_context;
    struct layer_visibility;
    class chart_style;
    class ephemeral_data;

    class feature_renderer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        feature_renderer(sdl::device& dev, const char* db_path, const chart_style& cs, const ephemeral_data& eph);
        ~feature_renderer();

        // Recompute visible features from database
        void update(double view_x_min, double view_y_min, double view_x_max, double view_y_max, double half_extent_y,
                    int viewport_height, double aspect_ratio);

        // Drain background builder results and rebuild SDF lines.
        // Returns true if new results were available.
        bool drain();

        // Get labels from the most recent drain (screen-space, overlap-eliminated)
        const std::vector<label_candidate>& labels() const;

        // Check if features need upload
        bool needs_upload() const;

        // Upload feature data to GPU
        void copy(sdl::copy_pass& pass);

        // Render visible features
        void render(sdl::render_pass& pass, const render_context& ctx, const glm::mat4& view_matrix) const;

        // Set which feature layers are visible
        void set_visibility(const layer_visibility& vis);

        // Force the next update() to re-submit a build request even if
        // the visible bbox / zoom haven't changed. Used when an
        // ephemeral data source swaps in fresh data and the screen
        // would otherwise keep showing the prior snapshot until the
        // next pan/zoom.
        void invalidate();

        // Set (or clear) the currently-selected feature. The builder re-runs
        // and emits a highlight overlay rendered on top of everything.
        void set_selection(std::optional<feature> sel);

        // Set (or clear) the active flight route. Triggers a rebuild.
        // A newly-set route starts in the selected state.
        void set_route(std::optional<flight_route> route);

        // Toggle the route's selected visualization (white line + halos
        // when selected; configured color + no halos when not).
        void set_route_selected(bool selected);
    };

} // namespace osect
