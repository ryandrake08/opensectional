#pragma once
#include <string>
#include <unordered_map>

namespace nasrbrowse
{
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

    public:
        // Build with hardcoded defaults, optionally overridden from INI
        chart_style(const std::string& ini_path,
                    chart_mode mode = chart_mode::vfr);

        // Look up a feature style by key (e.g. "airway_v", "sua_moa")
        const feature_style& get(const std::string& key) const;

        // Returns true if feature is visible at the given zoom level
        bool visible(const std::string& key, double zoom) const;
    };

} // namespace nasrbrowse
