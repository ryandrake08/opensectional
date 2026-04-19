#pragma once
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nasrbrowse
{
    struct airport;

    enum class chart_mode
    {
        vfr,
        ifr_low,
        ifr_high
    };

    struct feature_style
    {
        double min_zoom = 0.0;
        double max_zoom = 99.0;
        float r = 1.0F;
        float g = 1.0F;
        float b = 1.0F;
        float a = 1.0F;
        float line_width = 1.0F;
        float border_width = 1.0F;
        float dash_length = 0.0F;
        float gap_length = 0.0F;
    };

    class chart_style
    {
        std::unordered_map<std::string, feature_style> styles;

        const feature_style& get(const std::string& key) const;
        bool visible(const std::string& key, double zoom) const;

    public:
        // Build with hardcoded defaults, optionally overridden from INI
        chart_style(const std::string& ini_path,
                    chart_mode mode = chart_mode::vfr);

        // Airport visibility and style (keyed by airspace class)
        bool airport_visible(const airport& apt, double zoom) const;
        const feature_style& airport_style(const airport& apt) const;

        // Navaid visibility and style (keyed by nav_type)
        bool navaid_visible(const std::string& nav_type, double zoom) const;
        const feature_style& navaid_style(const std::string& nav_type) const;

        // Fix visibility (keyed by airway membership) and style (keyed by use_code)
        bool fix_visible(bool on_airway, double zoom) const;
        const feature_style& fix_style(const std::string& use_code) const;

        // Obstacle visibility and style (keyed by AGL height)
        bool obstacle_visible(int agl_ht, double zoom) const;
        const feature_style& obstacle_style(int agl_ht) const;

        // Airway visibility and style (keyed by airway ID)
        bool airway_visible(const std::string& awy_id, double zoom) const;
        const feature_style& airway_style(const std::string& awy_id) const;

        // MTR visibility and style
        bool mtr_visible(double zoom) const;
        const feature_style& mtr_style() const;

        // Runway visibility and style
        bool runway_visible(double zoom) const;
        const feature_style& runway_style() const;

        // Class airspace visibility and style (keyed by class + local_type)
        bool airspace_visible(const std::string& airspace_class,
                              const std::string& local_type, double zoom) const;
        const feature_style& airspace_style(const std::string& airspace_class,
                                             const std::string& local_type) const;

        // SUA visibility and style (keyed by sua_type)
        bool sua_visible(const std::string& sua_type, double zoom) const;
        const feature_style& sua_style(const std::string& sua_type) const;

        // ARTCC visibility and style (keyed by altitude)
        bool artcc_visible(const std::string& altitude, double zoom) const;
        const feature_style& artcc_style(const std::string& altitude) const;

        // ADIZ visibility and style
        bool adiz_visible(double zoom) const;
        const feature_style& adiz_style() const;

        // TFR visibility and style
        bool tfr_visible(double zoom) const;
        const feature_style& tfr_style() const;

        // PJA visibility and style (area vs point)
        bool pja_area_visible(double zoom) const;
        bool pja_point_visible(double zoom) const;
        const feature_style& pja_area_style() const;
        const feature_style& pja_point_style() const;

        // MAA visibility and style (area vs point)
        bool maa_area_visible(double zoom) const;
        bool maa_point_visible(double zoom) const;
        const feature_style& maa_area_style() const;
        const feature_style& maa_point_style() const;

        // RCO visibility and style
        bool rco_visible(double zoom) const;
        const feature_style& rco_style() const;

        // AWOS visibility and style
        bool awos_visible(double zoom) const;
        const feature_style& awos_style() const;

        // Route line and waypoint halo style (always visible)
        const feature_style& route_style() const;

        // Group-level early-out helpers: true if ANY variant in the group
        // is visible at this zoom. Used by builders to skip DB queries
        // entirely when nothing in the layer would render.
        bool any_airport_visible(double zoom) const;
        bool any_navaid_visible(double zoom) const;
        bool any_fix_visible(double zoom) const;
        bool any_obstacle_visible(double zoom) const;
        bool any_airway_visible(double zoom) const;
        bool any_airspace_visible(double zoom) const;
        bool any_sua_visible(double zoom) const;
        bool any_artcc_visible(double zoom) const;

        // SQL-filter helpers: each returns the set of DB-column values that
        // would render at this zoom, ready to drop into an IN (...) filter.
        // Returns nullopt when the "everything else" bucket is visible (caller
        // should skip filtering). Populated vector is the explicit allow-list.
        // Populated empty vector should not occur in practice — callers are
        // expected to guard with any_*_visible() first.
        using filter_list = std::optional<std::vector<std::string>>;

        filter_list visible_airport_classes(double zoom) const;       // APT airspace_class: B,C,D,E
        filter_list visible_airspace_values(double zoom) const;       // Union of CLASS and LOCAL_TYPE values
        filter_list visible_sua_types(double zoom) const;             // SUA_TYPE: RA,PA,WA,AA,NSA
    };

} // namespace nasrbrowse
