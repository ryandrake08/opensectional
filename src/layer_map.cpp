#include "layer_map.hpp"
#include "feature_renderer.hpp"
#include "label_renderer.hpp"
#include "map_view.hpp"
#include "nasr_database.hpp"
#include "pick_result.hpp"
#include "render_context.hpp"
#include "tile_renderer.hpp"
#include "ui_overlay.hpp"
#include "chart_style.hpp"
#include <cmath>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/types.hpp>
#include <vector>

// Pick box size in pixels (width and height of the pick region for point features)
constexpr int PICK_BOX_SIZE_PIXELS = 20;

// SDL left mouse button value
constexpr uint8_t BUTTON_LEFT = 1;

// Ray-casting point-in-polygon test
static bool point_in_ring(double px, double py,
                          const std::vector<nasrbrowse::airspace_point>& ring)
{
    bool inside = false;
    for(size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
    {
        double yi = ring[i].lat, yj = ring[j].lat;
        double xi = ring[i].lon, xj = ring[j].lon;
        if(((yi > py) != (yj > py)) &&
           (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
        {
            inside = !inside;
        }
    }
    return inside;
}

// Equirectangular distance check for circle features (PJA, MAA)
static bool point_in_circle_nm(double px, double py,
                                double cx, double cy, double radius_nm)
{
    double dlat = (py - cy) * 60.0;
    double dlon = (px - cx) * 60.0 * std::cos(cy * M_PI / 180.0);
    return (dlat * dlat + dlon * dlon) <= (radius_nm * radius_nm);
}


struct layer_map::impl
{
    nasrbrowse::map_view view;
    sdl::device& dev;
    int viewport_width;
    int viewport_height;
    bool needs_update;
    bool show_tiles;

    // Tile renderer for raster basemap
    nasrbrowse::tile_renderer tiles;

    // Vector feature renderer
    nasrbrowse::feature_renderer features;

    // Grid line vertex buffer (rebuilt on viewport change)
    std::vector<sdl::vertex_t2f_c4ub_v3f> grid_vertices;
    std::unique_ptr<sdl::buffer> grid_buffer;

    // Label renderer
    nasrbrowse::label_renderer labels;
    const sdl::sampler& text_sampler;

    // Pick state
    nasrbrowse::nasr_database pick_db;
    nasrbrowse::chart_style styles;
    nasrbrowse::layer_visibility vis;
    double cursor_ndc_x;
    double cursor_ndc_y;
    bool dragged;
    bool imgui_wants_mouse;

    impl(sdl::device& dev, const char* tile_path, const char* db_path,
         const nasrbrowse::chart_style& cs,
         sdl::text_engine& text_engine, sdl::font& font,
         sdl::font& outline_font, int outline_size,
         const sdl::sampler& text_sampler)
        : dev(dev)
        , viewport_width(0)
        , viewport_height(0)
        , needs_update(true)
        , show_tiles(true)
        , tiles(dev, tile_path)
        , features(dev, db_path, cs)
        , labels(dev, text_engine, font, outline_font, outline_size)
        , text_sampler(text_sampler)
        , pick_db(db_path)
        , styles(cs)
        , cursor_ndc_x(0)
        , cursor_ndc_y(0)
        , dragged(false)
        , imgui_wants_mouse(false)
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
        tiles.update(view_x_min(), view.view_y_min(),
                     view_x_max(), view.view_y_max(),
                     view.half_extent_y, viewport_height,
                     aspect_ratio());
        features.update(view_x_min(), view.view_y_min(),
                        view_x_max(), view.view_y_max(),
                        view.half_extent_y, viewport_height,
                        aspect_ratio());
    }

    nasrbrowse::pick_result pick_at(double ndc_x, double ndc_y)
    {
        using namespace nasrbrowse;

        double z = view.zoom_level(viewport_height);

        // NDC to Web Mercator meters
        double world_x = ndc_x * 2.0 * view.half_extent_y + view.center_x;
        double world_y = ndc_y * 2.0 * view.half_extent_y + view.center_y;
        double click_lon = mx_to_lon(world_x);
        double click_lat = my_to_lat(world_y);

        // Wrap longitude into [-180, 180] for database queries
        click_lon = std::fmod(click_lon + 180.0, 360.0);
        if(click_lon < 0) click_lon += 360.0;
        click_lon -= 180.0;

        // Point-feature pick box: convert pixel half-size to world coords
        double pick_half_ndc = (PICK_BOX_SIZE_PIXELS * 0.5) / viewport_height;
        double box_world_half = pick_half_ndc * 2.0 * view.half_extent_y;
        double box_lon_half = mx_to_lon(box_world_half);
        double box_lon_min = click_lon - box_lon_half;
        double box_lon_max = click_lon + box_lon_half;
        double box_lat_min = my_to_lat(world_y - box_world_half);
        double box_lat_max = my_to_lat(world_y + box_world_half);

        pick_result result;
        result.lon = click_lon;
        result.lat = click_lat;

        // Point features: query with pick box, all results are hits
        if(vis[layer_airports])
        {
            const auto& airports = pick_db.query_airports(
                box_lon_min, box_lat_min, box_lon_max, box_lat_max);
            for(const auto& apt : airports)
            {
                if(styles.airport_visible(apt, z))
                    result.features.push_back(apt);
            }
        }

        if(vis[layer_navaids])
        {
            const auto& navaids = pick_db.query_navaids(
                box_lon_min, box_lat_min, box_lon_max, box_lat_max);
            for(const auto& nav : navaids)
            {
                if(styles.navaid_visible(nav.nav_type, z))
                    result.features.push_back(nav);
            }
        }

        if(vis[layer_fixes])
        {
            const auto& fixes = pick_db.query_fixes(
                box_lon_min, box_lat_min, box_lon_max, box_lat_max);
            for(const auto& f : fixes)
            {
                // Naive: pick if visible under either airway or non-airway zoom key
                if(styles.fix_visible(true, z) || styles.fix_visible(false, z))
                    result.features.push_back(f);
            }
        }

        if(vis[layer_obstacles])
        {
            const auto& obstacles = pick_db.query_obstacles(
                box_lon_min, box_lat_min, box_lon_max, box_lat_max);
            for(const auto& obs : obstacles)
            {
                if(styles.obstacle_visible(obs.agl_ht, z))
                    result.features.push_back(obs);
            }
        }

        if(vis[layer_rco])
        {
            if(styles.rco_visible(z))
            {
                const auto& outlets = pick_db.query_comm_outlets(
                    box_lon_min, box_lat_min, box_lon_max, box_lat_max);
                for(const auto& c : outlets)
                    result.features.push_back(c);
            }
        }

        if(vis[layer_awos])
        {
            if(styles.awos_visible(z))
            {
                const auto& stations = pick_db.query_awos(
                    box_lon_min, box_lat_min, box_lon_max, box_lat_max);
                for(const auto& a : stations)
                    result.features.push_back(a);
            }
        }

        // Area features: query with click point as degenerate bbox, then test containment
        if(vis[layer_airspace])
        {
            const auto& airspaces = pick_db.query_class_airspace(
                click_lon, click_lat, click_lon, click_lat);
            for(const auto& a : airspaces)
            {
                if(!styles.airspace_visible(a.airspace_class, a.local_type, z))
                    continue;
                bool inside = false;
                for(const auto& ring : a.parts)
                {
                    if(point_in_ring(click_lon, click_lat, ring.points))
                    {
                        if(ring.is_hole)
                        {
                            inside = false;
                            break;
                        }
                        inside = true;
                    }
                }
                if(inside)
                    result.features.push_back(a);
            }
        }

        if(vis[layer_sua])
        {
            const auto& suas = pick_db.query_sua(
                click_lon, click_lat, click_lon, click_lat);
            for(const auto& s : suas)
            {
                if(!styles.sua_visible(s.sua_type, z))
                    continue;
                for(const auto& ring : s.parts)
                {
                    if(point_in_ring(click_lon, click_lat, ring.points))
                    {
                        result.features.push_back(s);
                        break;
                    }
                }
            }
        }

        if(vis[layer_artcc])
        {
            const auto& artccs = pick_db.query_artcc(
                click_lon, click_lat, click_lon, click_lat);
            for(const auto& a : artccs)
            {
                if(!styles.artcc_visible(a.altitude, z))
                    continue;
                if(point_in_ring(click_lon, click_lat, a.points))
                    result.features.push_back(a);
            }
        }

        if(vis[layer_adiz])
        {
            if(styles.adiz_visible(z))
            {
                const auto& adizs = pick_db.query_adiz(
                    click_lon, click_lat, click_lon, click_lat);
                for(const auto& a : adizs)
                {
                    for(const auto& part : a.parts)
                    {
                        if(point_in_ring(click_lon, click_lat, part))
                        {
                            result.features.push_back(a);
                            break;
                        }
                    }
                }
            }
        }

        if(vis[layer_pja])
        {
            if(styles.pja_area_visible(z))
            {
                const auto& pjas = pick_db.query_pjas(
                    click_lon, click_lat, click_lon, click_lat);
                for(const auto& p : pjas)
                {
                    if(point_in_circle_nm(click_lon, click_lat, p.lon, p.lat, p.radius_nm))
                        result.features.push_back(p);
                }
            }
        }

        if(vis[layer_maa])
        {
            bool maa_area_vis = styles.maa_area_visible(z);
            bool maa_point_vis = styles.maa_point_visible(z);
            if(maa_area_vis || maa_point_vis)
            {
                const auto& maas = pick_db.query_maas(
                    click_lon, click_lat, click_lon, click_lat);
                for(const auto& m : maas)
                {
                    if(!m.shape.empty() && maa_area_vis)
                    {
                        if(point_in_ring(click_lon, click_lat, m.shape))
                            result.features.push_back(m);
                    }
                    else if(m.radius_nm > 0)
                    {
                        bool is_area = (m.radius_nm > 0 && m.shape.empty());
                        if(is_area ? maa_area_vis : maa_point_vis)
                        {
                            if(point_in_circle_nm(click_lon, click_lat, m.lon, m.lat, m.radius_nm))
                                result.features.push_back(m);
                        }
                    }
                }
            }
        }

        return result;
    }
};

layer_map::layer_map(sdl::device& dev, const char* tile_path, const char* db_path,
                     const nasrbrowse::chart_style& cs,
                     sdl::text_engine& text_engine, sdl::font& font,
                     sdl::font& outline_font, int outline_size,
                     const sdl::sampler& text_sampler)
    : layer()
    , pimpl(new impl(dev, tile_path, db_path, cs, text_engine, font, outline_font, outline_size, text_sampler))
{
}

layer_map::~layer_map() = default;

void layer_map::set_visibility(const nasrbrowse::layer_visibility& vis)
{
    pimpl->show_tiles = vis[nasrbrowse::layer_basemap];
    pimpl->vis = vis;
    pimpl->features.set_visibility(vis);
    pimpl->needs_update = true;
}

void layer_map::set_imgui_wants_mouse(bool wants)
{
    pimpl->imgui_wants_mouse = wants;
}

double layer_map::zoom_level() const
{
    return pimpl->view.zoom_level(pimpl->viewport_height);
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
            auto result = pimpl->pick_at(pimpl->cursor_ndc_x, pimpl->cursor_ndc_y);
            std::cerr << "Pick at " << result.lon << ", " << result.lat
                      << ": " << result.features.size() << " feature(s)" << std::endl;
            for(const auto& f : result.features)
            {
                std::visit([](const auto& feature)
                {
                    using T = std::decay_t<decltype(feature)>;
                    if constexpr(std::is_same_v<T, nasrbrowse::airport>)
                        std::cerr << "  Airport: " << feature.arpt_id << " " << feature.arpt_name << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::navaid>)
                        std::cerr << "  Navaid: " << feature.nav_id << " " << feature.name << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::fix>)
                        std::cerr << "  Fix: " << feature.fix_id << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::obstacle>)
                        std::cerr << "  Obstacle: " << feature.oas_num << " " << feature.agl_ht << "ft AGL" << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::class_airspace>)
                        std::cerr << "  Airspace: Class " << feature.airspace_class << " " << feature.name << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::sua>)
                        std::cerr << "  SUA: " << feature.sua_type << " " << feature.name << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::artcc>)
                        std::cerr << "  ARTCC: " << feature.location_id << " " << feature.name << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::adiz>)
                        std::cerr << "  ADIZ: " << feature.name << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::maa>)
                        std::cerr << "  MAA: " << feature.name << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::pja>)
                        std::cerr << "  PJA: " << feature.name << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::awos>)
                        std::cerr << "  AWOS: " << feature.id << " " << feature.type << std::endl;
                    else if constexpr(std::is_same_v<T, nasrbrowse::comm_outlet>)
                        std::cerr << "  " << feature.comm_type << ": " << feature.outlet_name << " (" << feature.facility_name << ")" << std::endl;
                }, f);
            }
        }
    }
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
    pimpl->tiles.drain();

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

    bool result = pimpl->needs_update || pimpl->tiles.needs_upload()
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
    pimpl->tiles.copy(pass);
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
    if(pimpl->show_tiles)
    {
        pimpl->tiles.render(pass, ctx, view_matrix);
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
