#pragma once
#include <string>
#include <unordered_map>

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
        float line_width = 2.0F;
        float border_width = 1.0F;
        float dash_length = 0.0F;
        float gap_length = 0.0F;
    };

    class chart_style
    {
        std::unordered_map<std::string, feature_style> styles;
        feature_style fallback;

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
    };

} // namespace nasrbrowse
