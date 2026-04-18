#pragma once

// One feature_type per map feature type. Each subclass owns all
// per-type logic — pick, build, info, summary, etc. — in one place.
// The application holds a single vector of instances; map_widget,
// ui_overlay, and feature_builder all iterate that vector instead of
// branching on feature type.
//
// Stage 1 only exposes pick() + metadata. Later stages will add
// build/render, info_kv, summary, and variant-visitor helpers.

#include "feature_builder.hpp"   // build_context, polyline_data, polygon_fill_data
#include "geo_types.hpp"
#include "nasr_database.hpp"
#include "pick_result.hpp"
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nasrbrowse
{
    struct layer_visibility;
    class chart_style;

    // Bundles the inputs shared by every pick adapter: database,
    // style/visibility predicates, the click location in several forms,
    // and the point-feature pick radius. Built once per click by the
    // dispatcher.
    struct pick_context
    {
        nasr_database& db;
        const chart_style& styles;
        const layer_visibility& vis;
        double zoom;
        double click_lon;
        double click_lat;
        geo_bbox pick_box;     // padded around click for point features
        geo_bbox click_box;    // degenerate — click point only
        double pick_radius_nm; // half-diagonal of pick_box, for line hits
    };

    // Info-popup key/value rows produced by feature_type::info_kv().
    using kv_pair = std::pair<const char*, std::string>;
    using kv_list = std::vector<kv_pair>;

    class feature_type
    {
    public:
        virtual ~feature_type() = default;

        // UI checkbox label (e.g. "Airports").
        virtual const char* label() const = 0;

        // Value from the layer enum in ui_overlay.hpp.
        virtual int layer_id() const = 0;

        // Entity-type tag from FEATURE_SECTIONS in ui_sectioned_list
        // (e.g. "APT"). Groups pick results into their section.
        virtual const char* section_tag() const = 0;

        // True iff `f` is this layer's feature alternative.
        virtual bool owns(const feature& f) const = 0;

        // Push every feature hitting the click described by `ctx` onto
        // `out`. Stateless. Dispatcher has already confirmed this layer
        // is enabled.
        virtual void pick(const pick_context& ctx,
                          std::vector<feature>& out) const = 0;

        // One-line summary for the pick-selector popup and info title.
        // `f` is guaranteed to be of this layer's type (caller used
        // find_feature_type).
        virtual std::string summary(const feature& f) const = 0;

        // Info-popup rows.
        virtual kv_list info_kv(const feature& f) const = 0;

        // World lon/lat of this feature, when it's drawn as a single
        // point on the map (airports, navaids, etc). Returns nullopt
        // for area/line features. Used by the exact-point pick
        // short-circuit and by the default anchor_lonlat().
        virtual std::optional<std::pair<double, double>>
        point_coord(const feature& f) const
        {
            (void)f;
            return std::nullopt;
        }

        // Anchor for the info popup. Default: snap to the feature's
        // point coord when available, otherwise fall back to the click
        // point (area/line features). Layers with their own anchor
        // logic — e.g. area features that should still anchor at a
        // center coord — override this directly.
        virtual std::pair<double, double>
        anchor_lonlat(const feature& f,
                       double click_lon, double click_lat) const
        {
            if(auto c = point_coord(f)) return *c;
            return {click_lon, click_lat};
        }

        // Emit vertex geometry into ctx.poly[layer_id] (and labels into
        // ctx.labels) for every instance of this feature_type visible in
        // ctx.req's bbox / zoom / altitude filter.
        virtual void build(const build_context& ctx) const = 0;

        // Emit the selection-overlay geometry (halo + re-drawn icon/outline)
        // for the given feature. Called only when `f` was produced by
        // this feature_type's pick(). Subclasses with no highlight (e.g.
        // runway_type) override with an empty body.
        virtual void build_selection(const build_context& ctx,
                                      const feature& f,
                                      polyline_data& out,
                                      polygon_fill_data& fill_out) const = 0;
    };

    // Build the canonical ordered list of feature_types. Order controls
    // pick priority and UI checkbox order. Called once at startup.
    std::vector<std::unique_ptr<feature_type>> make_feature_types();

    // Find the feature_type that owns `f`. Throws std::logic_error if no
    // feature_type claims this variant alternative — a programming error,
    // since make_feature_types() registers a type for every alternative.
    const feature_type& find_feature_type(
        const std::vector<std::unique_ptr<feature_type>>& types,
        const feature& f);
}
