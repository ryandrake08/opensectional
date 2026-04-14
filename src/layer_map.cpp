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

// Equirectangular distance from a point to the nearest point on a segment, in NM
static double point_to_segment_nm(double px, double py,
                                   double ax, double ay,
                                   double bx, double by)
{
    double cos_lat = std::cos(py * M_PI / 180.0);
    double ax_nm = (ax - px) * 60.0 * cos_lat;
    double ay_nm = (ay - py) * 60.0;
    double bx_nm = (bx - px) * 60.0 * cos_lat;
    double by_nm = (by - py) * 60.0;

    double dx = bx_nm - ax_nm;
    double dy = by_nm - ay_nm;
    double len2 = dx * dx + dy * dy;

    double t = 0;
    if(len2 > 0)
    {
        t = -(ax_nm * dx + ay_nm * dy) / len2;
        if(t < 0) t = 0;
        else if(t > 1) t = 1;
    }

    double cx = ax_nm + t * dx;
    double cy = ay_nm + t * dy;
    return std::sqrt(cx * cx + cy * cy);
}


// Pick-feature classification and formatting helpers.
// NEARBY = point features; AIRWAYS = line features; AIRSPACE = area features.
enum pick_bucket { BUCKET_NEARBY, BUCKET_AIRWAYS, BUCKET_AIRSPACE, BUCKET_COUNT };

static pick_bucket feature_bucket(const nasrbrowse::pick_feature& f)
{
    return std::visit([](const auto& v) -> pick_bucket
    {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, nasrbrowse::airport>
                  || std::is_same_v<T, nasrbrowse::navaid>
                  || std::is_same_v<T, nasrbrowse::fix>
                  || std::is_same_v<T, nasrbrowse::obstacle>
                  || std::is_same_v<T, nasrbrowse::awos>
                  || std::is_same_v<T, nasrbrowse::comm_outlet>
                  || std::is_same_v<T, nasrbrowse::runway>)
            return BUCKET_NEARBY;
        else if constexpr(std::is_same_v<T, nasrbrowse::airway_segment>
                       || std::is_same_v<T, nasrbrowse::mtr_segment>)
            return BUCKET_AIRWAYS;
        else
            return BUCKET_AIRSPACE;
    }, f);
}

// True if the feature is rendered as a single point on the map (so exact-point
// short-circuit applies). Area features (including pja/maa circles and ring
// airspace) are not "points" here even though they have a center coordinate.
static bool feature_is_point(const nasrbrowse::pick_feature& f)
{
    return std::visit([](const auto& v) -> bool
    {
        using T = std::decay_t<decltype(v)>;
        return std::is_same_v<T, nasrbrowse::airport>
            || std::is_same_v<T, nasrbrowse::navaid>
            || std::is_same_v<T, nasrbrowse::fix>
            || std::is_same_v<T, nasrbrowse::obstacle>
            || std::is_same_v<T, nasrbrowse::awos>
            || std::is_same_v<T, nasrbrowse::comm_outlet>;
    }, f);
}

// Lon/lat of a point feature (only valid when feature_is_point is true).
static void feature_point_lonlat(const nasrbrowse::pick_feature& f,
                                  double& lon, double& lat)
{
    std::visit([&](const auto& v)
    {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, nasrbrowse::airport>
                  || std::is_same_v<T, nasrbrowse::navaid>
                  || std::is_same_v<T, nasrbrowse::fix>
                  || std::is_same_v<T, nasrbrowse::obstacle>
                  || std::is_same_v<T, nasrbrowse::awos>
                  || std::is_same_v<T, nasrbrowse::comm_outlet>)
        { lon = v.lon; lat = v.lat; }
        else
        { lon = 0.0; lat = 0.0; }
    }, f);
}

// One-line human-readable summary of a picked feature.
static std::string feature_summary(const nasrbrowse::pick_feature& f)
{
    std::ostringstream os;
    std::visit([&os](const auto& v)
    {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, nasrbrowse::airport>)
            os << "Airport: " << v.arpt_id << " " << v.arpt_name;
        else if constexpr(std::is_same_v<T, nasrbrowse::navaid>)
            os << "Navaid: " << v.nav_id << " " << v.name;
        else if constexpr(std::is_same_v<T, nasrbrowse::fix>)
            os << "Fix: " << v.fix_id;
        else if constexpr(std::is_same_v<T, nasrbrowse::obstacle>)
            os << "Obstacle: " << v.oas_num << " " << v.agl_ht << "ft AGL";
        else if constexpr(std::is_same_v<T, nasrbrowse::class_airspace>)
            os << "Airspace: Class " << v.airspace_class << " " << v.name;
        else if constexpr(std::is_same_v<T, nasrbrowse::sua>)
            os << "SUA: " << v.sua_type << " " << v.name;
        else if constexpr(std::is_same_v<T, nasrbrowse::artcc>)
            os << "ARTCC: " << v.location_id << " " << v.name;
        else if constexpr(std::is_same_v<T, nasrbrowse::adiz>)
            os << "ADIZ: " << v.name;
        else if constexpr(std::is_same_v<T, nasrbrowse::maa>)
        {
            os << "MAA: " << (!v.name.empty() ? v.name : v.maa_id);
            if(!v.type.empty()) os << " [" << v.type << "]";
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::pja>)
            os << "PJA: " << (!v.name.empty() ? v.name : v.pja_id);
        else if constexpr(std::is_same_v<T, nasrbrowse::awos>)
            os << "AWOS: " << v.id << " " << v.type;
        else if constexpr(std::is_same_v<T, nasrbrowse::comm_outlet>)
            os << v.comm_type << ": " << v.outlet_name << " (" << v.facility_name << ")";
        else if constexpr(std::is_same_v<T, nasrbrowse::airway_segment>)
            os << "Airway: " << v.awy_id << " " << v.from_point << "-" << v.to_point;
        else if constexpr(std::is_same_v<T, nasrbrowse::mtr_segment>)
            os << "MTR: " << v.mtr_id << " " << v.from_point << "-" << v.to_point;
        else if constexpr(std::is_same_v<T, nasrbrowse::runway>)
            os << "Runway: " << v.rwy_id;
    }, f);
    return os.str();
}

// Anchor lon/lat for a feature's info popup. Point features and
// circle-based area features use their own coord; line and polygon
// areas fall back to the click coord so the popup sits near where the
// user actually aimed.
static void feature_anchor_lonlat(const nasrbrowse::pick_feature& f,
                                   double click_lon, double click_lat,
                                   double& lon, double& lat)
{
    std::visit([&](const auto& v)
    {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, nasrbrowse::airport>
                  || std::is_same_v<T, nasrbrowse::navaid>
                  || std::is_same_v<T, nasrbrowse::fix>
                  || std::is_same_v<T, nasrbrowse::obstacle>
                  || std::is_same_v<T, nasrbrowse::awos>
                  || std::is_same_v<T, nasrbrowse::comm_outlet>)
        { lon = v.lon; lat = v.lat; }
        else if constexpr(std::is_same_v<T, nasrbrowse::pja>)
        { lon = v.lon; lat = v.lat; }
        else if constexpr(std::is_same_v<T, nasrbrowse::maa>)
        {
            // Point/circle MAAs have a center; shape-only MAAs don't.
            if(v.shape.empty()) { lon = v.lon; lat = v.lat; }
            else                { lon = click_lon; lat = click_lat; }
        }
        else
        { lon = click_lon; lat = click_lat; }
    }, f);
}

// Missing source fields stay empty; draw_info_imgui filters them out.
// Any substitution here would conflict with real values like "N/A" that
// might genuinely appear in NASR data.
static std::string nz(const std::string& s)
{
    return s;
}

static std::string fmt_int(int v)
{
    std::ostringstream os; os << v; return os.str();
}

static std::string fmt_dbl(double v, int prec)
{
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(prec);
    os << v;
    return os.str();
}

using kv_pair = std::pair<const char*, std::string>;
using kv_list = std::vector<kv_pair>;

// Key/value rows to display for each feature variant. Fields come
// directly from the already-fetched pick_feature; no extra JOINs.
static kv_list feature_kv(const nasrbrowse::pick_feature& f)
{
    kv_list rows;
    std::visit([&rows](const auto& v)
    {
        using T = std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T, nasrbrowse::airport>)
        {
            rows.push_back({"ID",              nz(v.arpt_id)});
            rows.push_back({"Name",            nz(v.arpt_name)});
            rows.push_back({"City",            nz(v.city)});
            rows.push_back({"State",           nz(v.state_code)});
            rows.push_back({"Elevation",       fmt_dbl(v.elev, 0) + " ft"});
            rows.push_back({"TPA",             nz(v.tpa)});
            rows.push_back({"Airspace class",  nz(v.airspace_class)});
            rows.push_back({"Tower type",      nz(v.twr_type_code)});
            rows.push_back({"ICAO ID",         nz(v.icao_id)});
            rows.push_back({"Ownership",       nz(v.ownership_type_code)});
            rows.push_back({"Facility use",    nz(v.facility_use_code)});
            rows.push_back({"Status",          nz(v.arpt_status)});
            rows.push_back({"Fuel types",      nz(v.fuel_types)});
            rows.push_back({"Mag var",         v.mag_varn.empty() ? std::string() : v.mag_varn + v.mag_hemis});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::navaid>)
        {
            rows.push_back({"ID",         nz(v.nav_id)});
            rows.push_back({"Name",       nz(v.name)});
            rows.push_back({"Type",       nz(v.nav_type)});
            rows.push_back({"City",       nz(v.city)});
            rows.push_back({"State",      nz(v.state_code)});
            rows.push_back({"Elevation",  fmt_dbl(v.elev, 0) + " ft"});
            rows.push_back({"Frequency",  nz(v.freq)});
            rows.push_back({"Channel",    nz(v.chan)});
            rows.push_back({"Power",      nz(v.pwr_output)});
            rows.push_back({"Hours",      nz(v.oper_hours)});
            rows.push_back({"Status",     nz(v.nav_status)});
            rows.push_back({"Low ARTCC",  nz(v.low_alt_artcc_id)});
            rows.push_back({"High ARTCC", nz(v.high_alt_artcc_id)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::fix>)
        {
            rows.push_back({"ID",          nz(v.fix_id)});
            rows.push_back({"State",       nz(v.state_code)});
            rows.push_back({"ICAO region", nz(v.icao_region_code)});
            rows.push_back({"Use code",    nz(v.use_code)});
            rows.push_back({"Low ARTCC",   nz(v.artcc_id_low)});
            rows.push_back({"High ARTCC",  nz(v.artcc_id_high)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::obstacle>)
        {
            rows.push_back({"OAS number",    nz(v.oas_num)});
            rows.push_back({"Type",          nz(v.obstacle_type)});
            rows.push_back({"Quantity",      fmt_int(v.quantity)});
            rows.push_back({"AGL height",    fmt_int(v.agl_ht) + " ft"});
            rows.push_back({"MSL height",    fmt_int(v.amsl_ht) + " ft"});
            rows.push_back({"Lighting",      nz(v.lighting)});
            rows.push_back({"Marking",       nz(v.marking)});
            rows.push_back({"City",          nz(v.city)});
            rows.push_back({"State",         nz(v.state)});
            rows.push_back({"Verify status", nz(v.verify_status)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::class_airspace>)
        {
            rows.push_back({"Name",       nz(v.name)});
            rows.push_back({"Class",      nz(v.airspace_class)});
            rows.push_back({"Local type", nz(v.local_type)});
            rows.push_back({"Ident",      nz(v.ident)});
            rows.push_back({"Sector",     nz(v.sector)});
            rows.push_back({"Upper",      v.upper_val.empty() && v.upper_desc.empty() ? std::string() : v.upper_desc + " " + v.upper_val});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::sua>)
        {
            rows.push_back({"Designator",             nz(v.designator)});
            rows.push_back({"Name",                   nz(v.name)});
            rows.push_back({"Type",                   nz(v.sua_type)});
            rows.push_back({"Upper",                  nz(v.upper_limit)});
            rows.push_back({"Lower",                  nz(v.lower_limit)});
            rows.push_back({"Controlling authority",  nz(v.controlling_authority)});
            rows.push_back({"Admin area",             nz(v.admin_area)});
            rows.push_back({"Activity",               nz(v.activity)});
            rows.push_back({"Working hours",          nz(v.working_hours)});
            rows.push_back({"Status",                 nz(v.status)});
            rows.push_back({"ICAO compliant",         nz(v.icao_compliant)});
            rows.push_back({"Legal note",             nz(v.legal_note)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::artcc>)
        {
            rows.push_back({"Location ID",   nz(v.location_id)});
            rows.push_back({"Name",          nz(v.name)});
            rows.push_back({"Altitude band", nz(v.altitude)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::adiz>)
        {
            rows.push_back({"Name",          nz(v.name)});
            rows.push_back({"Location",      nz(v.location)});
            rows.push_back({"Working hours", nz(v.working_hours)});
            rows.push_back({"Military",      nz(v.military)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::maa>)
        {
            rows.push_back({"ID",      nz(v.maa_id)});
            rows.push_back({"Name",    nz(v.name)});
            rows.push_back({"Type",    nz(v.type)});
            rows.push_back({"Min alt", nz(v.min_alt)});
            rows.push_back({"Max alt", nz(v.max_alt)});
            rows.push_back({"Radius",  fmt_dbl(v.radius_nm, 1) + " NM"});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::pja>)
        {
            rows.push_back({"ID",           nz(v.pja_id)});
            rows.push_back({"Name",         nz(v.name)});
            rows.push_back({"Max altitude", nz(v.max_altitude)});
            rows.push_back({"Radius",       fmt_dbl(v.radius_nm, 1) + " NM"});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::awos>)
        {
            rows.push_back({"ID",            nz(v.id)});
            rows.push_back({"Type",          nz(v.type)});
            rows.push_back({"City",          nz(v.city)});
            rows.push_back({"State",         nz(v.state_code)});
            rows.push_back({"Elevation",     fmt_dbl(v.elev, 0) + " ft"});
            rows.push_back({"Phone",         nz(v.phone_no)});
            rows.push_back({"Second phone",  nz(v.second_phone_no)});
            rows.push_back({"Commissioned",  nz(v.commissioned_date)});
            rows.push_back({"Remark",        nz(v.remark)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::comm_outlet>)
        {
            rows.push_back({"Outlet name",   nz(v.outlet_name)});
            rows.push_back({"Type",          nz(v.comm_type)});
            rows.push_back({"Facility ID",   nz(v.facility_id)});
            rows.push_back({"Facility name", nz(v.facility_name)});
            rows.push_back({"Hours",         nz(v.opr_hrs)});
            rows.push_back({"Status",        nz(v.comm_status_code)});
            rows.push_back({"City",          nz(v.city)});
            rows.push_back({"State",         nz(v.state_code)});
            rows.push_back({"Remark",        nz(v.remark)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::airway_segment>)
        {
            rows.push_back({"ID",            nz(v.awy_id)});
            rows.push_back({"Location",      nz(v.awy_location)});
            rows.push_back({"From",          nz(v.from_point)});
            rows.push_back({"To",            nz(v.to_point)});
            rows.push_back({"MEA",           nz(v.min_enroute_alt)});
            rows.push_back({"Mag crs/dist", nz(v.mag_course_dist)});
            rows.push_back({"Gap flag",      nz(v.gap_flag)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::mtr_segment>)
        {
            rows.push_back({"ID",         nz(v.mtr_id)});
            rows.push_back({"Route type", nz(v.route_type_code)});
            rows.push_back({"From",       nz(v.from_point)});
            rows.push_back({"To",         nz(v.to_point)});
        }
        else if constexpr(std::is_same_v<T, nasrbrowse::runway>)
        {
            rows.push_back({"ID",      nz(v.rwy_id)});
            rows.push_back({"Site no", nz(v.site_no)});
        }
    }, f);
    return rows;
}

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
    std::vector<nasrbrowse::pick_feature> features;
};

// Info-popup state: details for a single selected feature.
struct info_popup_state
{
    bool open = false;
    int session_id = 0;
    int warmup_frames = 0;
    double anchor_lon = 0.0;
    double anchor_lat = 0.0;
    nasrbrowse::pick_feature feature{nasrbrowse::airport{}};
};

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

    // Pick state
    nasrbrowse::nasr_database pick_db;
    nasrbrowse::chart_style styles;
    nasrbrowse::layer_visibility vis;
    double cursor_ndc_x;
    double cursor_ndc_y;
    bool dragged;
    bool imgui_wants_mouse;

    pick_popup_state pick_popup;
    info_popup_state info_popup;

    impl(sdl::device& dev, const char* tile_path, const char* db_path,
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
        geo_bbox pick_box{
            click_lon - box_lon_half,
            my_to_lat(world_y - box_world_half),
            click_lon + box_lon_half,
            my_to_lat(world_y + box_world_half)};
        geo_bbox click_box{click_lon, click_lat, click_lon, click_lat};

        pick_result result;
        result.lon = click_lon;
        result.lat = click_lat;

        // Point features: query with pick box, all results are hits
        if(vis[layer_airports])
        {
            const auto& airports = pick_db.query_airports(pick_box);
            for(const auto& apt : airports)
            {
                if(styles.airport_visible(apt, z))
                    result.features.push_back(apt);
            }
        }

        if(vis[layer_navaids] && vis.altitude.any())
        {
            const auto& navaids = pick_db.query_navaids(pick_box);
            for(const auto& nav : navaids)
            {
                if(!styles.navaid_visible(nav.nav_type, z)) continue;
                bool keep = (nav.is_low && vis.altitude.show_low)
                         || (nav.is_high && vis.altitude.show_high);
                if(keep) result.features.push_back(nav);
            }
        }

        if(vis[layer_fixes] && vis.altitude.any())
        {
            const auto& fixes = pick_db.query_fixes(pick_box);
            for(const auto& f : fixes)
            {
                if(!(styles.fix_visible(true, z) || styles.fix_visible(false, z))) continue;
                bool keep = (f.is_low && vis.altitude.show_low)
                         || (f.is_high && vis.altitude.show_high);
                if(keep) result.features.push_back(f);
            }
        }

        if(vis[layer_obstacles] && vis.altitude.low_enabled())
        {
            const auto& obstacles = pick_db.query_obstacles(pick_box);
            for(const auto& obs : obstacles)
            {
                if(styles.obstacle_visible(obs.agl_ht, z))
                    result.features.push_back(obs);
            }
        }

        if(vis[layer_rco] && vis.altitude.low_enabled())
        {
            if(styles.rco_visible(z))
            {
                const auto& outlets = pick_db.query_comm_outlets(pick_box);
                for(const auto& c : outlets)
                    result.features.push_back(c);
            }
        }

        if(vis[layer_awos] && vis.altitude.low_enabled())
        {
            if(styles.awos_visible(z))
            {
                const auto& stations = pick_db.query_awos(pick_box);
                for(const auto& a : stations)
                    result.features.push_back(a);
            }
        }

        // Area features: query with click point as degenerate bbox, then test containment
        if(vis[layer_airspace] && vis.altitude.low_enabled())
        {
            const auto& airspaces = pick_db.query_class_airspace(click_box);
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

        if(vis[layer_sua] && vis.altitude.any())
        {
            const auto& suas = pick_db.query_sua(click_box);
            for(const auto& s : suas)
            {
                if(!styles.sua_visible(s.sua_type, z))
                    continue;
                bool inside = false;
                bool any_ring_in_band = false;
                for(const auto& ring : s.parts)
                {
                    if(point_in_ring(click_lon, click_lat, ring.points))
                    {
                        if(ring.is_hole)
                        {
                            inside = false;
                            break;
                        }
                        inside = true;
                        if(vis.altitude.overlaps(ring.lower_ft_msl, ring.upper_ft_msl))
                            any_ring_in_band = true;
                    }
                }
                if(inside && any_ring_in_band)
                    result.features.push_back(s);
            }
        }

        if(vis[layer_artcc] && vis.altitude.any())
        {
            const auto& artccs = pick_db.query_artcc(click_box);
            for(const auto& a : artccs)
            {
                if(!styles.artcc_visible(a.altitude, z))
                    continue;
                if(!altitude_filter_allows(vis.altitude, artcc_bands(a.altitude)))
                    continue;
                if(point_in_ring(click_lon, click_lat, a.points))
                    result.features.push_back(a);
            }
        }

        if(vis[layer_adiz])
        {
            if(styles.adiz_visible(z))
            {
                const auto& adizs = pick_db.query_adiz(click_box);
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

        if(vis[layer_pja] && vis.altitude.any())
        {
            bool pja_area_vis = styles.pja_area_visible(z);
            bool pja_point_vis = styles.pja_point_visible(z);
            if(pja_area_vis || pja_point_vis)
            {
                // Query with the pick box so point PJAs (radius_nm == 0)
                // whose R-tree bbox is degenerate still come back.
                const auto& pjas = pick_db.query_pjas(pick_box);
                for(const auto& p : pjas)
                {
                    int upper = p.max_altitude_ft_msl > 0 ? p.max_altitude_ft_msl
                                                          : altitude_filter::UNLIMITED_FT;
                    if(!vis.altitude.overlaps(0, upper)) continue;
                    bool is_point = (p.radius_nm <= 0);
                    if(is_point ? !pja_point_vis : !pja_area_vis) continue;
                    bool hit;
                    if(is_point)
                        hit = (p.lon >= pick_box.lon_min && p.lon <= pick_box.lon_max
                            && p.lat >= pick_box.lat_min && p.lat <= pick_box.lat_max);
                    else
                        hit = point_in_circle_nm(click_lon, click_lat, p.lon, p.lat, p.radius_nm);
                    if(hit)
                        result.features.push_back(p);
                }
            }
        }

        if(vis[layer_maa] && vis.altitude.low_enabled())
        {
            bool maa_area_vis = styles.maa_area_visible(z);
            bool maa_point_vis = styles.maa_point_visible(z);
            if(maa_area_vis || maa_point_vis)
            {
                const auto& maas = pick_db.query_maas(click_box);
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

        // Line features: query with pick box, test point-to-segment distance
        double pick_radius_nm = (pick_box.lat_max - pick_box.lat_min) * 0.5 * 60.0;

        if(vis[layer_airways] && vis.altitude.any())
        {
            const auto& airways = pick_db.query_airways(pick_box);
            for(const auto& seg : airways)
            {
                if(!styles.airway_visible(seg.awy_id, z))
                    continue;
                if(!altitude_filter_allows(vis.altitude, airway_bands(seg.awy_id)))
                    continue;
                double d = point_to_segment_nm(click_lon, click_lat,
                    seg.from_lon, seg.from_lat, seg.to_lon, seg.to_lat);
                if(d <= pick_radius_nm)
                    result.features.push_back(seg);
            }
        }

        if(vis[layer_mtrs] && vis.altitude.any())
        {
            if(styles.mtr_visible(z))
            {
                const auto& mtrs = pick_db.query_mtrs(pick_box);
                for(const auto& seg : mtrs)
                {
                    if(!altitude_filter_allows(vis.altitude, mtr_bands(seg.route_type_code)))
                        continue;
                    double d = point_to_segment_nm(click_lon, click_lat,
                        seg.from_lon, seg.from_lat, seg.to_lon, seg.to_lat);
                    if(d <= pick_radius_nm)
                        result.features.push_back(seg);
                }
            }
        }

        if(vis[layer_runways] && vis.altitude.low_enabled())
        {
            if(styles.runway_visible(z))
            {
                const auto& runways = pick_db.query_runways(pick_box);
                for(const auto& rwy : runways)
                {
                    double d = point_to_segment_nm(click_lon, click_lat,
                        rwy.end1_lon, rwy.end1_lat, rwy.end2_lon, rwy.end2_lat);
                    if(d <= pick_radius_nm)
                        result.features.push_back(rwy);
                }
            }
        }

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
    void open_info_popup(const nasrbrowse::pick_feature& f,
                          double click_lon, double click_lat)
    {
        double alon, alat;
        feature_anchor_lonlat(f, click_lon, click_lat, alon, alat);
        info_popup.open = true;
        ++info_popup.session_id;
        info_popup.warmup_frames = 2;
        info_popup.anchor_lon = alon;
        info_popup.anchor_lat = alat;
        info_popup.feature = f;
        features.set_selection(f);
        needs_update = true;
    }

    void close_info_popup()
    {
        info_popup.open = false;
        features.set_selection(std::nullopt);
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
        const nasrbrowse::pick_feature* exact_hit = nullptr;
        for(const auto& f : result.features)
        {
            if(!feature_is_point(f)) continue;
            double flon, flat;
            feature_point_lonlat(f, flon, flat);
            float fx, fy;
            world_to_pixel(flon, flat, fx, fy);
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

            const char* headers[BUCKET_COUNT] = {"NEARBY", "AIRWAYS", "AIRSPACE"};
            const nasrbrowse::pick_feature* selected = nullptr;

            for(int b = 0; b < BUCKET_COUNT; ++b)
            {
                bool any = false;
                for(const auto& f : pick_popup.features)
                    if(feature_bucket(f) == b) { any = true; break; }
                if(!any) continue;

                ImGui::TextDisabled("%s", headers[b]);
                int idx = 0;
                for(const auto& f : pick_popup.features)
                {
                    if(feature_bucket(f) != b) continue;
                    std::string summary = feature_summary(f);
                    char id[32];
                    std::snprintf(id, sizeof(id), "##pick_item_%d_%d", b, idx++);
                    ImGui::PushID(id);
                    if(ImGui::Selectable(summary.c_str(), false))
                        selected = &f;
                    ImGui::PopID();
                }
            }

            if(selected)
            {
                open_info_popup(*selected, pick_popup.click_lon, pick_popup.click_lat);
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
        std::string title = feature_summary(info_popup.feature);
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
        kv_list rows = feature_kv(info_popup.feature);
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
                     const nasrbrowse::chart_style& cs,
                     sdl::text_engine& text_engine, sdl::font& font,
                     sdl::font& outline_font,
                     const sdl::sampler& text_sampler)
    : layer()
    , pimpl(new impl(dev, tile_path, db_path, cs, text_engine, font, outline_font, text_sampler))
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
