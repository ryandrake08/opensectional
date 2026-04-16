#include "layer_map.hpp"
#include "feature_type.hpp"
#include "feature_renderer.hpp"
#include "label_renderer.hpp"
#include "map_view.hpp"
#include "nasr_database.hpp"
#include "pick_result.hpp"
#include "render_context.hpp"
#include "tile_renderer.hpp"
#include "ui_overlay.hpp"
#include "ui_sectioned_list.hpp"
#include "chart_style.hpp"
#include <cmath>
#include <glm/ext/matrix_transform.hpp>
#include <imgui.h>
#include <iostream>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/types.hpp>
#include <sstream>
#include <string>
#include <vector>

// Pick box size in pixels (width and height of the pick region for point features)
constexpr int PICK_BOX_SIZE_PIXELS = 20;

// Exact-point short-circuit radius: a single point feature this close to the
// cursor skips the candidate-selector popup and logs directly.
constexpr int PICK_BOX_EXACT_SIZE_PIXELS = 4;

// Padding between the click anchor and the selector popup, in pixels.
constexpr float PICK_POPUP_ANCHOR_PADDING = 12.0F;

// SDL left mouse button value
constexpr uint8_t BUTTON_LEFT = 1;

namespace
{
    // Selector-popup state. Lives in layer_map::impl.
    struct pick_popup_state
    {
        bool open = false;
        int session_id = 0;
        int warmup_frames = 0;  // ImGui hides a freshly-created auto-resize
                                // window for its first frame while it measures
                                // content. Force a couple of extra renders so
                                // the popup actually appears without needing a
                                // subsequent mouse event to wake the main loop.
        double click_lon = 0.0;
        double click_lat = 0.0;
        std::vector<nasrbrowse::feature> features;
    };

    // Info-popup state: details for a single selected feature.
    struct info_popup_state
    {
        bool open = false;
        int session_id = 0;
        int warmup_frames = 0;
        double anchor_lon = 0.0;
        double anchor_lat = 0.0;
        nasrbrowse::feature feature{nasrbrowse::airport{}};
    };
}

struct layer_map::impl
{
    nasrbrowse::map_view view;
    sdl::device& dev;
    int viewport_width;
    int viewport_height;
    bool needs_update;
    bool show_tiles;

    // Tile renderer for raster basemap (null if no basemap was provided)
    std::unique_ptr<nasrbrowse::tile_renderer> tiles;

    // Vector feature renderer
    nasrbrowse::feature_renderer features;

    // Grid line vertex buffer (rebuilt on viewport change)
    std::vector<sdl::vertex_t2f_c4ub_v3f> grid_vertices;
    std::unique_ptr<sdl::buffer> grid_buffer;

    // Label renderer
    nasrbrowse::label_renderer labels;
    const sdl::sampler& text_sampler;

    // Pick state. The database is owned by main() — layer_map borrows it
    // for click-time picks and search-result navigation.
    nasrbrowse::nasr_database& pick_db;
    nasrbrowse::chart_style styles;
    nasrbrowse::layer_visibility vis;
    double cursor_ndc_x;
    double cursor_ndc_y;
    bool dragged;
    bool imgui_wants_mouse;

    pick_popup_state pick_popup;
    info_popup_state info_popup;

    // All toggleable feature_types. Built once at construction; pick_at
    // and the UI iterate this vector. See feature_type.hpp.
    std::vector<std::unique_ptr<nasrbrowse::feature_type>> feature_types;

    impl(sdl::device& dev, const char* tile_path, const char* db_path,
         nasrbrowse::nasr_database& pick_db,
         const nasrbrowse::chart_style& cs,
         sdl::text_engine& text_engine, sdl::font& font,
         sdl::font& outline_font,
         const sdl::sampler& text_sampler)
        : dev(dev)
        , viewport_width(0)
        , viewport_height(0)
        , needs_update(true)
        , show_tiles(true)
        , tiles(tile_path ? std::unique_ptr<nasrbrowse::tile_renderer>(new nasrbrowse::tile_renderer(dev, tile_path)) : nullptr)
        , features(dev, db_path, cs)
        , labels(dev, text_engine, font, outline_font)
        , text_sampler(text_sampler)
        , pick_db(pick_db)
        , styles(cs)
        , cursor_ndc_x(0)
        , cursor_ndc_y(0)
        , dragged(false)
        , imgui_wants_mouse(false)
        , feature_types(nasrbrowse::make_feature_types())
    {
    }

    double aspect_ratio() const
    {
        return static_cast<double>(viewport_width) / viewport_height;
    }

    double view_x_min() const { return view.center_x - view.half_extent_y * aspect_ratio(); }
    double view_x_max() const { return view.center_x + view.half_extent_y * aspect_ratio(); }

    void rebuild_grid()
    {
        grid_vertices.clear();

        double vx_min = view_x_min();
        double vx_max = view_x_max();
        double vy_min = view.view_y_min();
        double vy_max = view.view_y_max();
        double range_x = vx_max - vx_min;
        double range_y = vy_max - vy_min;

        // Normalize coordinates to -0.5..0.5 for rendering
        auto to_ndc_x = [&](double mx) -> float
        {
            return static_cast<float>((mx - vx_min) / range_x - 0.5) * aspect_ratio();
        };
        auto to_ndc_y = [&](double my) -> float
        {
            return static_cast<float>((my - vy_min) / range_y - 0.5);
        };

        // Draw grid lines at regular lat/lon intervals
        // Choose interval based on zoom level
        double approx_zoom = view.zoom_level(viewport_height);
        double lon_step, lat_step;
        if(approx_zoom < 3)
        {
            lon_step = 30.0;
            lat_step = 30.0;
        }
        else if(approx_zoom < 5)
        {
            lon_step = 10.0;
            lat_step = 10.0;
        }
        else if(approx_zoom < 7)
        {
            lon_step = 5.0;
            lat_step = 5.0;
        }
        else if(approx_zoom < 9)
        {
            lon_step = 1.0;
            lat_step = 1.0;
        }
        else
        {
            lon_step = 0.5;
            lat_step = 0.5;
        }

        // Grid line color: dark gray, semi-transparent
        uint8_t r = 80, g = 80, b = 80, a = 160;

        // Longitude lines (vertical)
        double lon_min = nasrbrowse::mx_to_lon(vx_min);
        double lon_max = nasrbrowse::mx_to_lon(vx_max);
        double lon_start = std::floor(lon_min / lon_step) * lon_step;
        for(double lon = lon_start; lon <= lon_max; lon += lon_step)
        {
            double mx = nasrbrowse::lon_to_mx(lon);
            float nx = to_ndc_x(mx);
            float ny0 = to_ndc_y(vy_min);
            float ny1 = to_ndc_y(vy_max);
            grid_vertices.push_back({ 0, 0, r, g, b, a, nx, ny0, 0 });
            grid_vertices.push_back({ 0, 0, r, g, b, a, nx, ny1, 0 });
        }

        // Latitude lines (horizontal, non-uniform spacing in Mercator)
        double lat_min = nasrbrowse::my_to_lat(vy_min);
        double lat_max = nasrbrowse::my_to_lat(vy_max);
        if(lat_min < -nasrbrowse::MAX_LATITUDE) lat_min = -nasrbrowse::MAX_LATITUDE;
        if(lat_max > nasrbrowse::MAX_LATITUDE) lat_max = nasrbrowse::MAX_LATITUDE;
        double lat_start = std::floor(lat_min / lat_step) * lat_step;
        for(double lat = lat_start; lat <= lat_max; lat += lat_step)
        {
            double my = nasrbrowse::lat_to_my(lat);
            float ny = to_ndc_y(my);
            float nx0 = to_ndc_x(vx_min);
            float nx1 = to_ndc_x(vx_max);
            grid_vertices.push_back({ 0, 0, r, g, b, a, nx0, ny, 0 });
            grid_vertices.push_back({ 0, 0, r, g, b, a, nx1, ny, 0 });
        }

        needs_update = true;
    }

    void update_tiles()
    {
        if(tiles)
        {
            tiles->update(view_x_min(), view.view_y_min(),
                          view_x_max(), view.view_y_max(),
                          view.half_extent_y, viewport_height,
                          aspect_ratio());
        }
        features.update(view_x_min(), view.view_y_min(),
                        view_x_max(), view.view_y_max(),
                        view.half_extent_y, viewport_height,
                        aspect_ratio());
    }

    nasrbrowse::pick_result pick_at(double ndc_x, double ndc_y)
    {
        using namespace nasrbrowse;

        // NDC → Web Mercator → lon/lat, wrapping lon to [-180, 180] for DB.
        double world_x = ndc_x * 2.0 * view.half_extent_y + view.center_x;
        double world_y = ndc_y * 2.0 * view.half_extent_y + view.center_y;
        double click_lon = mx_to_lon(world_x);
        double click_lat = my_to_lat(world_y);
        click_lon = std::fmod(click_lon + 180.0, 360.0);
        if(click_lon < 0) click_lon += 360.0;
        click_lon -= 180.0;

        // Point-feature pick box: pixel half-size → world coords.
        double pick_half_ndc = (PICK_BOX_SIZE_PIXELS * 0.5) / viewport_height;
        double box_world_half = pick_half_ndc * 2.0 * view.half_extent_y;
        double box_lon_half = mx_to_lon(box_world_half);
        geo_bbox pick_box{
            click_lon - box_lon_half,
            my_to_lat(world_y - box_world_half),
            click_lon + box_lon_half,
            my_to_lat(world_y + box_world_half)};
        geo_bbox click_box{click_lon, click_lat, click_lon, click_lat};

        nasrbrowse::pick_context ctx{
            pick_db, styles, vis,
            view.zoom_level(viewport_height),
            click_lon, click_lat,
            pick_box, click_box,
            (pick_box.lat_max - pick_box.lat_min) * 0.5 * 60.0};

        pick_result result;
        result.lon = click_lon;
        result.lat = click_lat;
        for(const auto& obj : feature_types)
            if(vis[obj->layer_id()])
                obj->pick(ctx, result.features);
        return result;
    }

    // Convert a world lon/lat into pixel coordinates relative to the
    // ImGui display origin (top-left). Handles antimeridian wrap by
    // shifting lon to be closest to the current view center.
    void world_to_pixel(double lon, double lat,
                        float& px, float& py) const
    {
        double world_x = nasrbrowse::lon_to_mx(lon);
        // Unwrap to stay near view.center_x
        constexpr double W = 2.0 * nasrbrowse::HALF_CIRCUMFERENCE;
        while(world_x - view.center_x > nasrbrowse::HALF_CIRCUMFERENCE)
            world_x -= W;
        while(view.center_x - world_x > nasrbrowse::HALF_CIRCUMFERENCE)
            world_x += W;
        double world_y = nasrbrowse::lat_to_my(lat);

        double ndc_x = (world_x - view.center_x) / (2.0 * view.half_extent_y);
        double ndc_y = (world_y - view.center_y) / (2.0 * view.half_extent_y);

        px = static_cast<float>(ndc_x * viewport_height + viewport_width * 0.5);
        py = static_cast<float>((0.5 - ndc_y) * viewport_height);
    }

    // Compute where to place a popup anchored at world coord (lon, lat).
    // Returns true and fills pos/pivot when the anchor is on-screen.
    // Returns false when off-screen (popup should dismiss).
    bool compute_popup_anchor(double lon, double lat,
                              ImVec2& pos, ImVec2& pivot) const
    {
        float ax, ay;
        world_to_pixel(lon, lat, ax, ay);
        if(ax < 0 || ax > viewport_width || ay < 0 || ay > viewport_height)
            return false;

        bool right  = ax >= viewport_width * 0.5F;
        bool bottom = ay >= viewport_height * 0.5F;

        pos.x = ax + (right  ? -PICK_POPUP_ANCHOR_PADDING : PICK_POPUP_ANCHOR_PADDING);
        pos.y = ay + (bottom ? -PICK_POPUP_ANCHOR_PADDING : PICK_POPUP_ANCHOR_PADDING);
        pivot = ImVec2(right ? 1.0F : 0.0F, bottom ? 1.0F : 0.0F);
        return true;
    }

    // Open (or replace) the info popup showing details for a selected feature.
    void open_info_popup(const nasrbrowse::feature& f,
                          double click_lon, double click_lat)
    {
        auto [alon, alat] = nasrbrowse::find_feature_type(feature_types, f)
                              .anchor_lonlat(f, click_lon, click_lat);
        info_popup.open = true;
        ++info_popup.session_id;
        info_popup.warmup_frames = 2;
        info_popup.anchor_lon = alon;
        info_popup.anchor_lat = alat;
        info_popup.feature = f;
        features.set_selection(f);
        update_tiles();
        needs_update = true;
    }

    void close_info_popup()
    {
        info_popup.open = false;
        features.set_selection(std::nullopt);
        update_tiles();
        needs_update = true;
    }

    // Handle a completed left-click pick: run pick_at, apply exact-point
    // short-circuit, and either open the info popup directly or open
    // the selector popup.
    void handle_pick()
    {
        pick_popup.open = false;
        pick_popup.features.clear();

        auto result = pick_at(cursor_ndc_x, cursor_ndc_y);

        // Exact-point short-circuit: count point features within the
        // exact-pick pixel radius of the click.
        const double exact_threshold_px = PICK_BOX_EXACT_SIZE_PIXELS;
        int exact_count = 0;
        const nasrbrowse::feature* exact_hit = nullptr;
        for(const auto& f : result.features)
        {
            auto coord = nasrbrowse::find_feature_type(feature_types, f).point_coord(f);
            if(!coord) continue;
            float fx, fy;
            world_to_pixel(coord->first, coord->second, fx, fy);
            float cx, cy;
            world_to_pixel(result.lon, result.lat, cx, cy);
            double dx = fx - cx, dy = fy - cy;
            if(std::sqrt(dx * dx + dy * dy) <= exact_threshold_px)
            {
                ++exact_count;
                exact_hit = &f;
            }
        }

        if(exact_count == 1)
        {
            open_info_popup(*exact_hit, result.lon, result.lat);
            return;
        }

        // Opening the selector supersedes any stale info popup.
        close_info_popup();
        pick_popup.open = true;
        ++pick_popup.session_id;
        pick_popup.warmup_frames = 2;
        pick_popup.click_lon = result.lon;
        pick_popup.click_lat = result.lat;
        pick_popup.features = std::move(result.features);
        needs_update = true;
    }

    // Draw the selector popup during an ImGui frame. Called from the main
    // loop between new_frame and end_frame. Also auto-dismisses if the
    // anchor point has scrolled out of the viewport.
    // Returns true if another render frame is needed.
    bool draw_pick_imgui()
    {
        if(!pick_popup.open) return false;

        ImVec2 pos, pivot;
        if(!compute_popup_anchor(pick_popup.click_lon, pick_popup.click_lat, pos, pivot))
        {
            pick_popup.open = false;
            return false;
        }

        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        ImGui::SetNextWindowBgAlpha(0.9F);
        ImGui::SetNextWindowSizeConstraints(ImVec2(220, 0), ImVec2(FLT_MAX, viewport_height * 0.8F));
        char window_id[48];
        std::snprintf(window_id, sizeof(window_id), "##pick_selector_%d", pick_popup.session_id);
        ImGui::Begin(window_id, nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoTitleBar);

        // Header row: lat/long on the left, [X] on the right
        ImGui::Text("Lat, Long: %.5f, %.5f", pick_popup.click_lat, pick_popup.click_lon);
        ImGui::SameLine();
        // Align close button to right edge of the window content region
        float btn_w = ImGui::GetFrameHeight();
        float avail = ImGui::GetContentRegionAvail().x;
        if(avail > btn_w)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btn_w);
        if(ImGui::SmallButton("X##pick_close"))
        {
            pick_popup.open = false;
            ImGui::End();
            return false;
        }

        if(!pick_popup.features.empty())
        {
            ImGui::Separator();

            // Group pick features by their canonical feature-section tag.
            std::vector<nasrbrowse::ui_section> sections(nasrbrowse::FEATURE_SECTION_COUNT);
            std::vector<std::vector<int>> section_feature_index(nasrbrowse::FEATURE_SECTION_COUNT);
            for(std::size_t s = 0; s < nasrbrowse::FEATURE_SECTION_COUNT; ++s)
                sections[s].header = nasrbrowse::FEATURE_SECTIONS[s].header;
            for(int i = 0; i < static_cast<int>(pick_popup.features.size()); ++i)
            {
                const auto& f = pick_popup.features[i];
                const auto& t = nasrbrowse::find_feature_type(feature_types, f);
                int s = nasrbrowse::feature_section_index(t.section_tag());
                if(s < 0) continue;
                sections[s].items.push_back(t.summary(f));
                section_feature_index[s].push_back(i);
            }

            auto picked = nasrbrowse::draw_sectioned_selectable_list(sections);
            if(picked)
            {
                int fi = section_feature_index[picked->first][picked->second];
                open_info_popup(pick_popup.features[fi],
                                pick_popup.click_lon, pick_popup.click_lat);
                pick_popup.open = false;
                pick_popup.features.clear();
            }
        }

        ImGui::End();

        bool need_more = pick_popup.warmup_frames > 0;
        if(need_more) --pick_popup.warmup_frames;
        return need_more;
    }

    // Max width of the value column (in pixels); anything longer wraps.
    static constexpr float INFO_VALUE_WRAP_PX = 280.0F;

    // Draw the info popup for the currently selected feature. Returns true
    // while warming up (first measurement frame).
    bool draw_info_imgui()
    {
        if(!info_popup.open) return false;

        ImVec2 pos, pivot;
        if(!compute_popup_anchor(info_popup.anchor_lon, info_popup.anchor_lat, pos, pivot))
        {
            close_info_popup();
            return false;
        }

        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        ImGui::SetNextWindowBgAlpha(0.9F);
        ImGui::SetNextWindowSizeConstraints(ImVec2(220, 0), ImVec2(FLT_MAX, viewport_height * 0.9F));
        char window_id[48];
        std::snprintf(window_id, sizeof(window_id), "##info_popup_%d", info_popup.session_id);
        ImGui::Begin(window_id, nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoTitleBar);

        // Title row: feature summary on the left, [X] on the right.
        const auto& L = nasrbrowse::find_feature_type(feature_types, info_popup.feature);
        std::string title = L.summary(info_popup.feature);
        ImGui::TextUnformatted(title.c_str());
        ImGui::SameLine();
        float btn_w = ImGui::GetFrameHeight();
        float avail = ImGui::GetContentRegionAvail().x;
        if(avail > btn_w)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btn_w);
        if(ImGui::SmallButton("X##info_close"))
        {
            close_info_popup();
            ImGui::End();
            return false;
        }
        ImGui::Separator();

        // Two-column layout: fixed-width keys (auto-fit) + fixed-width
        // wrapped values. Using an ImGui table gives us per-cell wrapping
        // while keeping the key column aligned.
        nasrbrowse::kv_list rows = L.info_kv(info_popup.feature);
        const ImGuiTableFlags flags =
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
        if(ImGui::BeginTable("info_kv", 2, flags))
        {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, INFO_VALUE_WRAP_PX);

            float line_h = ImGui::GetTextLineHeight();
            for(const auto& [key, value] : rows)
            {
                if(value.empty()) continue;  // hide empty-source fields
                ImGui::TableNextRow();

                // Measure the wrapped value so we can vertically center
                // the key against it when the value spans multiple lines.
                ImVec2 val_size = ImGui::CalcTextSize(
                    value.c_str(), nullptr, false, INFO_VALUE_WRAP_PX);
                float row_h = val_size.y > line_h ? val_size.y : line_h;

                ImGui::TableSetColumnIndex(0);
                float y_offset = (row_h - line_h) * 0.5F;
                if(y_offset > 0.0F)
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + y_offset);
                ImGui::TextUnformatted(key);

                ImGui::TableSetColumnIndex(1);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + INFO_VALUE_WRAP_PX);
                // Right-justify the value: push its starting X so the
                // rendered (wrapped) block ends at the column's right edge.
                float val_w = val_size.x;
                if(val_w < INFO_VALUE_WRAP_PX)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (INFO_VALUE_WRAP_PX - val_w));
                ImGui::TextUnformatted(value.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTable();
        }

        ImGui::End();

        bool need_more = info_popup.warmup_frames > 0;
        if(need_more) --info_popup.warmup_frames;
        return need_more;
    }
};

layer_map::layer_map(sdl::device& dev, const char* tile_path, const char* db_path,
                     nasrbrowse::nasr_database& db,
                     const nasrbrowse::chart_style& cs,
                     sdl::text_engine& text_engine, sdl::font& font,
                     sdl::font& outline_font,
                     const sdl::sampler& text_sampler)
    : layer()
    , pimpl(new impl(dev, tile_path, db_path, db, cs, text_engine, font, outline_font, text_sampler))
{
}

layer_map::~layer_map() = default;

void layer_map::set_visibility(const nasrbrowse::layer_visibility& vis)
{
    pimpl->show_tiles = vis[nasrbrowse::layer_basemap];
    pimpl->vis = vis;
    pimpl->features.set_visibility(vis);
    pimpl->needs_update = true;
    // Kick a requery in case the altitude filter invalidated the cache —
    // otherwise nothing re-runs until the next input event.
    pimpl->update_tiles();
}

void layer_map::focus_on_hit(const nasrbrowse::search_hit& hit)
{
    auto bbox = pimpl->pick_db.get_hit_bbox(hit);
    if(!bbox) return;

    auto& d = *pimpl;
    double cx = (bbox->lon_min + bbox->lon_max) * 0.5;
    double cy = (bbox->lat_min + bbox->lat_max) * 0.5;
    d.view.center_x = nasrbrowse::lon_to_mx(cx);
    d.view.center_y = nasrbrowse::lat_to_my(cy);

    // Degenerate bbox (point entity) → use a fixed close-in zoom level so
    // the feature is guaranteed visible under chart_style's per-type
    // min_zoom thresholds (navaids appear by ~z9, airports by ~z7, fixes
    // by ~z10 — zoom 12 covers all of them comfortably).
    constexpr int POINT_FOCUS_ZOOM = 12;
    bool is_point = (bbox->lon_min == bbox->lon_max &&
                     bbox->lat_min == bbox->lat_max);
    if(is_point)
    {
        d.view.zoom_to_level(d.viewport_height, POINT_FOCUS_ZOOM);
    }
    else
    {
        // Fit the bbox with 20% padding. Use the larger of the required
        // vertical / horizontal extents (the latter scaled by aspect ratio).
        double half_height_m =
            (nasrbrowse::lat_to_my(bbox->lat_max) -
             nasrbrowse::lat_to_my(bbox->lat_min)) * 0.5;
        double half_width_m =
            (nasrbrowse::lon_to_mx(bbox->lon_max) -
             nasrbrowse::lon_to_mx(bbox->lon_min)) * 0.5;
        double needed = std::max(half_height_m,
                                  half_width_m / d.aspect_ratio());
        d.view.half_extent_y = needed * 1.2;
        // Force clamp by setting same extent through zoom(1.0).
        d.view.zoom(1.0, d.viewport_height);
    }

    d.needs_update = true;
    d.update_tiles();
}

void layer_map::set_imgui_wants_mouse(bool wants)
{
    pimpl->imgui_wants_mouse = wants;
}

double layer_map::zoom_level() const
{
    return pimpl->view.zoom_level(pimpl->viewport_height);
}

const std::vector<std::unique_ptr<nasrbrowse::feature_type>>&
layer_map::feature_types() const
{
    return pimpl->feature_types;
}

void layer_map::on_button_input(sdl::input_button_t button, sdl::input_action_t action, sdl::input_mod_t)
{
    if(static_cast<uint8_t>(button) != BUTTON_LEFT)
        return;

    if(static_cast<int>(action) != 0) // press
    {
        pimpl->dragged = false;
    }
    else // release
    {
        if(!pimpl->dragged && !pimpl->imgui_wants_mouse)
        {
            pimpl->handle_pick();
        }
    }
}

bool layer_map::draw_imgui()
{
    // Both popups can coexist in theory, but in practice the selector is
    // dismissed as soon as a feature is chosen. Always draw both so either
    // one's warmup frames propagate upward.
    bool a = pimpl->draw_pick_imgui();
    bool b = pimpl->draw_info_imgui();
    return a || b;
}

void layer_map::on_cursor_position(double xpos, double ypos)
{
    pimpl->cursor_ndc_x = xpos;
    pimpl->cursor_ndc_y = ypos;
}

void layer_map::on_key_input(sdl::input_key_t key, sdl::input_action_t action, sdl::input_mod_t)
{
    if(action != sdl::input_action::release)
    {
        switch(static_cast<int>(key))
        {
        case 'w':
        case 'W':
            pimpl->view.pan(0.0, 0.1, pimpl->aspect_ratio());
            pimpl->rebuild_grid();
            pimpl->update_tiles();
            break;
        case 's':
        case 'S':
            pimpl->view.pan(0.0, -0.1, pimpl->aspect_ratio());
            pimpl->rebuild_grid();
            pimpl->update_tiles();
            break;
        case 'a':
        case 'A':
            pimpl->view.pan(-0.1, 0.0, pimpl->aspect_ratio());
            pimpl->rebuild_grid();
            pimpl->update_tiles();
            break;
        case 'd':
        case 'D':
            pimpl->view.pan(0.1, 0.0, pimpl->aspect_ratio());
            pimpl->rebuild_grid();
            pimpl->update_tiles();
            break;
        case 'r':
        case 'R':
        {
            int z = static_cast<int>(pimpl->view.zoom_level(pimpl->viewport_height)) + 1;
            pimpl->view.zoom_to_level(pimpl->viewport_height, z);
            pimpl->rebuild_grid();
            pimpl->update_tiles();
            break;
        }
        case 'f':
        case 'F':
        {
            int z = static_cast<int>(pimpl->view.zoom_level(pimpl->viewport_height)) - 1;
            pimpl->view.zoom_to_level(pimpl->viewport_height, z);
            pimpl->rebuild_grid();
            pimpl->update_tiles();
            break;
        }
        }
    }
}

void layer_map::on_drag_input(const std::vector<sdl::input_button_t>&, double xdelta, double ydelta)
{
    pimpl->dragged = true;
    // Convert NDC delta to meters
    double dx_meters = -xdelta * pimpl->view.half_extent_y * 2.0;
    double dy_meters = -ydelta * pimpl->view.half_extent_y * 2.0;
    pimpl->view.pan_meters(dx_meters, dy_meters);
    pimpl->rebuild_grid();
    pimpl->update_tiles();
}

void layer_map::on_scroll(double, double yoffset)
{
    double factor = (yoffset > 0) ? 0.9 : 1.0 / 0.9;
    double wx = pimpl->cursor_ndc_x * 2.0 * pimpl->view.half_extent_y + pimpl->view.center_x;
    double wy = pimpl->cursor_ndc_y * 2.0 * pimpl->view.half_extent_y + pimpl->view.center_y;
    pimpl->view.zoom_at(factor, wx, wy, pimpl->viewport_height);
    pimpl->rebuild_grid();
    pimpl->update_tiles();
}

void layer_map::on_resize(float normalized_viewport_width, int viewport_height_pixels)
{
    pimpl->viewport_width = static_cast<int>(normalized_viewport_width * viewport_height_pixels);
    pimpl->viewport_height = viewport_height_pixels;
    pimpl->rebuild_grid();
    pimpl->update_tiles();
}

bool layer_map::on_update()
{
    if(pimpl->tiles) pimpl->tiles->drain();

    bool new_candidates = pimpl->features.drain();
    if(new_candidates)
    {
        pimpl->labels.set_candidates(pimpl->features.labels());
    }

    // Reproject labels only when the view changed or new candidates arrived
    if(pimpl->needs_update || new_candidates)
    {
        pimpl->labels.update_positions(
            pimpl->view.center_x, pimpl->view.center_y,
            pimpl->view.half_extent_y,
            pimpl->viewport_width, pimpl->viewport_height,
            pimpl->vis);
    }

    bool result = pimpl->needs_update
        || (pimpl->tiles && pimpl->tiles->needs_upload())
        || pimpl->features.needs_upload() || pimpl->labels.needs_upload();

    if(result)
    {
        pimpl->needs_update = false;
    }
    return result;
}

void layer_map::on_copy(sdl::copy_pass& pass)
{
    if(!pimpl->grid_vertices.empty())
    {
        pimpl->grid_buffer.reset();
        auto buf = pass.create_and_upload_buffer(pimpl->dev, sdl::buffer_usage::vertex, pimpl->grid_vertices);
        pimpl->grid_buffer = std::make_unique<sdl::buffer>(std::move(buf));
    }
    if(pimpl->tiles) pimpl->tiles->copy(pass);
    pimpl->features.copy(pass);
    pimpl->labels.copy(pass, pimpl->dev);
}

void layer_map::on_render(sdl::render_pass& pass, const nasrbrowse::render_context& ctx) const
{
    float s = static_cast<float>(1.0 / (2.0 * pimpl->view.half_extent_y));
    float cx = static_cast<float>(pimpl->view.center_x);
    float cy = static_cast<float>(pimpl->view.center_y);
    glm::mat4 view_matrix = glm::scale(glm::mat4(1.0F), glm::vec3(s, s, 1.0F)) *
                             glm::translate(glm::mat4(1.0F), glm::vec3(-cx, -cy, 0.0F));

    // Render tiles (textured pass)
    if(pimpl->show_tiles && pimpl->tiles)
    {
        pimpl->tiles->render(pass, ctx, view_matrix);
    }

    pimpl->features.render(pass, ctx, view_matrix);

    // Render labels (text pass)
    pimpl->labels.render(pass, ctx, pimpl->text_sampler,
                         pimpl->viewport_width, pimpl->viewport_height);

    // Render grid (line pass)
    if(ctx.current_pass == nasrbrowse::render_pass_id::trianglelist_0)
    {
        if(pimpl->grid_buffer && pimpl->grid_buffer->count() > 0)
        {
            sdl::uniform_buffer uniforms;
            uniforms.projection_matrix = ctx.projection_matrix;

            pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
            pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
            pass.bind_vertex_buffer(*pimpl->grid_buffer);
            pass.draw(pimpl->grid_buffer->count());
        }
    }
}
