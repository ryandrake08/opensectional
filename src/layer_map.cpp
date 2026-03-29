#include "layer_map.hpp"
#include "feature_renderer.hpp"
#include "map_view.hpp"
#include "render_context.hpp"
#include "tile_renderer.hpp"
#include "ui_overlay.hpp"
#include "chart_style.hpp"
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/types.hpp>
#include <vector>

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

    impl(sdl::device& dev, const char* tile_path, const char* db_path,
         const nasrbrowse::chart_style& cs)
        : dev(dev)
        , viewport_width(0)
        , viewport_height(0)
        , needs_update(true)
        , show_tiles(true)
        , tiles(dev, tile_path)
        , features(dev, db_path, cs)
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
                     viewport_height, aspect_ratio());
        features.update(view_x_min(), view.view_y_min(),
                        view_x_max(), view.view_y_max(),
                        view.half_extent_y, viewport_height,
                        aspect_ratio());
    }
};

layer_map::layer_map(sdl::device& dev, const char* tile_path, const char* db_path,
                     const nasrbrowse::chart_style& cs)
    : layer()
    , pimpl(new impl(dev, tile_path, db_path, cs))
{
}

layer_map::~layer_map() = default;

void layer_map::set_visibility(const nasrbrowse::layer_visibility& vis)
{
    pimpl->show_tiles = vis[nasrbrowse::layer_basemap];
    pimpl->features.set_visibility(vis);
    pimpl->needs_update = true;
}

double layer_map::zoom_level() const
{
    return pimpl->view.zoom_level(pimpl->viewport_height);
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
    pimpl->view.zoom(factor, pimpl->viewport_height);
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
    bool result = pimpl->needs_update || pimpl->tiles.needs_upload() ||
                  pimpl->features.needs_upload();

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
}

void layer_map::on_render(sdl::render_pass& pass, const nasrbrowse::render_context& ctx) const
{
    // Render tiles (textured pass)
    if(pimpl->show_tiles)
    {
        pimpl->tiles.render(pass, ctx);
    }

    // Render vector features (line pass)
    pimpl->features.render(pass, ctx);

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
