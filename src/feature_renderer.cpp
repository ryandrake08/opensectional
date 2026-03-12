#include "feature_renderer.hpp"
#include "map_view.hpp"
#include "nasr_database.hpp"
#include "render_context.hpp"
#include <cmath>
#include <glm/ext/matrix_transform.hpp>
#include <sdl/buffer.hpp>
#include <sdl/copy_pass.hpp>
#include <sdl/device.hpp>
#include <sdl/render_pass.hpp>
#include <sdl/types.hpp>
#include <vector>

namespace nasrbrowse
{
    struct feature_renderer::impl
    {
        sdl::device& dev;
        nasr_database db;

        // Current view state
        double center_x, center_y, half_extent_y;
        int viewport_height;

        // Cached query bbox (in lon/lat). We pad the query region and only
        // re-query when the viewport moves outside the padded area.
        double query_lon_min, query_lat_min, query_lon_max, query_lat_max;
        double last_zoom;
        bool has_cached_query;

        // Vertex buffers for each feature type
        std::vector<sdl::vertex_t2f_c4ub_v3f> airport_vertices;
        std::vector<sdl::vertex_t2f_c4ub_v3f> navaid_vertices;
        std::vector<sdl::vertex_t2f_c4ub_v3f> fix_vertices;
        std::vector<sdl::vertex_t2f_c4ub_v3f> airway_vertices;
        std::vector<sdl::vertex_t2f_c4ub_v3f> airspace_vertices;

        std::unique_ptr<sdl::buffer> airport_buffer;
        std::unique_ptr<sdl::buffer> navaid_buffer;
        std::unique_ptr<sdl::buffer> fix_buffer;
        std::unique_ptr<sdl::buffer> airway_buffer;
        std::unique_ptr<sdl::buffer> airspace_buffer;

        bool dirty;

        impl(sdl::device& dev, const char* db_path)
            : dev(dev)
            , db(db_path)
            , center_x(0)
            , center_y(0)
            , half_extent_y(HALF_CIRCUMFERENCE)
            , viewport_height(0)
            , query_lon_min(0)
            , query_lat_min(0)
            , query_lon_max(0)
            , query_lat_max(0)
            , last_zoom(-1)
            , has_cached_query(false)
            , dirty(false)
        {
        }

        double zoom_level() const
        {
            double meters_per_pixel = (half_extent_y * 2.0) / viewport_height;
            double world_size = 2.0 * HALF_CIRCUMFERENCE;
            return std::log2(world_size / (256.0 * meters_per_pixel));
        }

        // Check if we need to re-query the database
        bool needs_requery(double lon_min, double lat_min,
                           double lon_max, double lat_max) const
        {
            if(!has_cached_query)
            {
                return true;
            }

            // Re-query if zoom changed significantly
            double z = zoom_level();
            if(std::abs(z - last_zoom) > 0.5)
            {
                return true;
            }

            // Re-query if viewport moved outside the cached query region
            return lon_min < query_lon_min || lon_max > query_lon_max ||
                   lat_min < query_lat_min || lat_max > query_lat_max;
        }

        void build_vertices(double lon_min, double lat_min,
                             double lon_max, double lat_max)
        {
            double z = zoom_level();

            // Pad the query region by 50% so we don't re-query on small pans
            double lon_pad = (lon_max - lon_min) * 0.5;
            double lat_pad = (lat_max - lat_min) * 0.5;
            double qlon_min = lon_min - lon_pad;
            double qlat_min = lat_min - lat_pad;
            double qlon_max = lon_max + lon_pad;
            double qlat_max = lat_max + lat_pad;

            query_lon_min = qlon_min;
            query_lat_min = qlat_min;
            query_lon_max = qlon_max;
            query_lat_max = qlat_max;
            last_zoom = z;
            has_cached_query = true;

            airport_vertices.clear();
            navaid_vertices.clear();
            fix_vertices.clear();
            airway_vertices.clear();
            airspace_vertices.clear();

            build_airport_vertices(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_navaid_vertices(qlon_min, qlat_min, qlon_max, qlat_max, z);
            build_airway_vertices(qlon_min, qlat_min, qlon_max, qlat_max, z);

            if(z >= 7.0)
            {
                build_fix_vertices(qlon_min, qlat_min, qlon_max, qlat_max);
            }

            if(z >= 4.0)
            {
                build_airspace_vertices(qlon_min, qlat_min, qlon_max, qlat_max, z);
            }

            dirty = true;
        }

        // Generate a diamond shape at a point in Web Mercator meters
        void add_diamond(std::vector<sdl::vertex_t2f_c4ub_v3f>& verts,
                         double mx, double my, float radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            float x = static_cast<float>(mx);
            float y = static_cast<float>(my);
            // Diamond as 4 line segments
            verts.push_back({0, 0, r, g, b, a, x - radius, y, 0});
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
            verts.push_back({0, 0, r, g, b, a, x + radius, y, 0});
            verts.push_back({0, 0, r, g, b, a, x + radius, y, 0});
            verts.push_back({0, 0, r, g, b, a, x, y - radius, 0});
            verts.push_back({0, 0, r, g, b, a, x, y - radius, 0});
            verts.push_back({0, 0, r, g, b, a, x - radius, y, 0});
        }

        // Generate a triangle shape at a point
        void add_triangle(std::vector<sdl::vertex_t2f_c4ub_v3f>& verts,
                          double mx, double my, float radius,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a)
        {
            float x = static_cast<float>(mx);
            float y = static_cast<float>(my);
            float h = radius * 0.866F; // sqrt(3)/2
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
            verts.push_back({0, 0, r, g, b, a, x + h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x + h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x - h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x - h, y - radius * 0.5F, 0});
            verts.push_back({0, 0, r, g, b, a, x, y + radius, 0});
        }

        void build_airport_vertices(double lon_min, double lat_min,
                                     double lon_max, double lat_max, double z)
        {
            const auto& airports = db.query_airports(lon_min, lat_min, lon_max, lat_max);

            // Symbol size in meters, shrinks with zoom
            float radius = static_cast<float>(half_extent_y * 0.008);

            for(const auto& apt : airports)
            {
                // At low zoom, only show towered airports
                if(z < 6.0 && apt.twr_type_code == "NON-ATCT")
                {
                    continue;
                }

                double mx = lon_to_mx(apt.lon);
                double my = lat_to_my(apt.lat);

                uint8_t r, g, b, a = 255;
                if(apt.site_type_code == "H")
                {
                    // Heliport: magenta
                    r = 200; g = 50; b = 200;
                }
                else if(apt.twr_type_code != "NON-ATCT")
                {
                    // Towered: blue
                    r = 60; g = 120; b = 255;
                }
                else
                {
                    // Untowered: magenta
                    r = 200; g = 50; b = 200;
                }

                add_diamond(airport_vertices, mx, my, radius, r, g, b, a);
            }
        }

        void build_navaid_vertices(double lon_min, double lat_min,
                                    double lon_max, double lat_max, double z)
        {
            if(z < 5.0)
            {
                return;
            }

            const auto& navaids = db.query_navaids(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(half_extent_y * 0.006);

            for(const auto& nav : navaids)
            {
                double mx = lon_to_mx(nav.lon);
                double my = lat_to_my(nav.lat);

                // VOR/VORTAC: green, NDB: red
                uint8_t r, g, b;
                if(nav.nav_type == "NDB" || nav.nav_type == "NDB/DME")
                {
                    r = 200; g = 80; b = 80;
                }
                else
                {
                    r = 80; g = 200; b = 80;
                }

                add_diamond(navaid_vertices, mx, my, radius, r, g, b, 255);
            }
        }

        void build_fix_vertices(double lon_min, double lat_min,
                                 double lon_max, double lat_max)
        {
            const auto& fixes = db.query_fixes(lon_min, lat_min, lon_max, lat_max);
            float radius = static_cast<float>(half_extent_y * 0.003);

            for(const auto& fix : fixes)
            {
                double mx = lon_to_mx(fix.lon);
                double my = lat_to_my(fix.lat);
                add_triangle(fix_vertices, mx, my, radius, 140, 140, 140, 200);
            }
        }

        void build_airway_vertices(double lon_min, double lat_min,
                                    double lon_max, double lat_max, double z)
        {
            if(z < 5.0)
            {
                return;
            }

            const auto& airways = db.query_airways(lon_min, lat_min, lon_max, lat_max);

            for(const auto& seg : airways)
            {
                double mx0 = lon_to_mx(seg.from_lon);
                double my0 = lat_to_my(seg.from_lat);
                double mx1 = lon_to_mx(seg.to_lon);
                double my1 = lat_to_my(seg.to_lat);

                // Victor airways (V): blue, Jet (J): dark gray, others: magenta
                uint8_t r, g, b;
                if(!seg.awy_id.empty() && seg.awy_id[0] == 'V')
                {
                    r = 80; g = 80; b = 180;
                }
                else if(!seg.awy_id.empty() && seg.awy_id[0] == 'J')
                {
                    r = 100; g = 100; b = 100;
                }
                else
                {
                    r = 180; g = 80; b = 180;
                }

                float x0 = static_cast<float>(mx0);
                float y0 = static_cast<float>(my0);
                float x1 = static_cast<float>(mx1);
                float y1 = static_cast<float>(my1);
                airway_vertices.push_back({0, 0, r, g, b, 160, x0, y0, 0});
                airway_vertices.push_back({0, 0, r, g, b, 160, x1, y1, 0});
            }
        }

        void build_airspace_vertices(double lon_min, double lat_min,
                                      double lon_max, double lat_max, double z)
        {
            const auto& airspaces = db.query_class_airspace(lon_min, lat_min, lon_max, lat_max);

            for(const auto& arsp : airspaces)
            {
                // At low zoom, only show Class B
                if(z < 6.0 && arsp.airspace_class != "B")
                {
                    continue;
                }
                // At medium zoom, show B and C
                if(z < 7.0 && arsp.airspace_class != "B" && arsp.airspace_class != "C")
                {
                    continue;
                }

                uint8_t r, g, b, a;
                if(arsp.airspace_class == "B")
                {
                    r = 60; g = 120; b = 220; a = 200;
                }
                else if(arsp.airspace_class == "C")
                {
                    r = 180; g = 60; b = 180; a = 200;
                }
                else if(arsp.airspace_class == "D")
                {
                    r = 60; g = 120; b = 220; a = 160;
                }
                else
                {
                    // Class E
                    r = 180; g = 60; b = 180; a = 120;
                }

                for(const auto& ring : arsp.parts)
                {
                    for(size_t i = 0; i + 1 < ring.size(); i++)
                    {
                        float x0 = static_cast<float>(lon_to_mx(ring[i].lon));
                        float y0 = static_cast<float>(lat_to_my(ring[i].lat));
                        float x1 = static_cast<float>(lon_to_mx(ring[i + 1].lon));
                        float y1 = static_cast<float>(lat_to_my(ring[i + 1].lat));
                        airspace_vertices.push_back({0, 0, r, g, b, a, x0, y0, 0});
                        airspace_vertices.push_back({0, 0, r, g, b, a, x1, y1, 0});
                    }
                }
            }
        }
    };

    feature_renderer::feature_renderer(sdl::device& dev, const char* db_path)
        : pimpl(new impl(dev, db_path))
    {
    }

    feature_renderer::~feature_renderer() = default;

    bool feature_renderer::update(double vx_min, double vy_min,
                                   double vx_max, double vy_max,
                                   int viewport_height, double)
    {
        pimpl->center_x = (vx_min + vx_max) * 0.5;
        pimpl->center_y = (vy_min + vy_max) * 0.5;
        pimpl->half_extent_y = (vy_max - vy_min) * 0.5;
        pimpl->viewport_height = viewport_height;

        // Convert view bounds to lon/lat for database query
        double lon_min = mx_to_lon(vx_min);
        double lon_max = mx_to_lon(vx_max);
        double lat_min = my_to_lat(vy_min);
        double lat_max = my_to_lat(vy_max);

        if(pimpl->needs_requery(lon_min, lat_min, lon_max, lat_max))
        {
            pimpl->build_vertices(lon_min, lat_min, lon_max, lat_max);
            return true;
        }
        return false;
    }

    bool feature_renderer::needs_upload() const
    {
        return pimpl->dirty;
    }

    void feature_renderer::prepare(size_t& size) const
    {
        auto add = [&](const auto& verts)
        {
            if(!verts.empty())
            {
                size += verts.size() * sizeof(sdl::vertex_t2f_c4ub_v3f);
            }
        };

        add(pimpl->airspace_vertices);
        add(pimpl->airway_vertices);
        add(pimpl->fix_vertices);
        add(pimpl->navaid_vertices);
        add(pimpl->airport_vertices);
    }

    void feature_renderer::copy(sdl::copy_pass& pass)
    {
        auto upload = [&](auto& verts, auto& buffer)
        {
            buffer.reset();
            if(!verts.empty())
            {
                auto buf = pass.create_and_upload_buffer(
                    pimpl->dev, sdl::buffer_usage::vertex, verts);
                buffer = std::make_unique<sdl::buffer>(std::move(buf));
            }
        };

        upload(pimpl->airspace_vertices, pimpl->airspace_buffer);
        upload(pimpl->airway_vertices, pimpl->airway_buffer);
        upload(pimpl->fix_vertices, pimpl->fix_buffer);
        upload(pimpl->navaid_vertices, pimpl->navaid_buffer);
        upload(pimpl->airport_vertices, pimpl->airport_buffer);
        pimpl->dirty = false;
    }

    void feature_renderer::render(sdl::render_pass& pass,
                                   const render_context& ctx) const
    {
        if(ctx.current_pass != render_pass_id::trianglelist_0)
        {
            return;
        }

        // View matrix: same approach as tile_renderer
        float s = static_cast<float>(1.0 / (2.0 * pimpl->half_extent_y));
        float cx = static_cast<float>(pimpl->center_x);
        float cy = static_cast<float>(pimpl->center_y);

        glm::mat4 view_matrix = glm::scale(glm::mat4(1.0F), glm::vec3(s, s, 1.0F)) *
                                 glm::translate(glm::mat4(1.0F), glm::vec3(-cx, -cy, 0.0F));

        sdl::uniform_buffer uniforms;
        uniforms.projection_matrix = ctx.projection_matrix;
        uniforms.view_matrix = view_matrix;

        // Draw each buffer in back-to-front order
        auto draw = [&](const auto& buffer)
        {
            if(buffer && buffer->count() > 0)
            {
                pass.push_vertex_uniforms(0, &uniforms, sizeof(uniforms));
                pass.push_fragment_uniforms(0, &uniforms, sizeof(uniforms));
                pass.bind_vertex_buffer(*buffer);
                pass.draw(buffer->count());
            }
        };

        draw(pimpl->airspace_buffer);
        draw(pimpl->airway_buffer);
        draw(pimpl->fix_buffer);
        draw(pimpl->navaid_buffer);
        draw(pimpl->airport_buffer);
    }

} // namespace nasrbrowse
