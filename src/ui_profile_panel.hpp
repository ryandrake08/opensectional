#pragma once

#include <optional>
#include <string>

namespace osect
{
    struct terrain_profile;

    class ui_profile_panel
    {
        bool open_ = false;

    public:
        bool open() const;
        void open_drawer();
        void toggle();
        void draw(const terrain_profile* profile, std::optional<std::string> error, bool has_active_route,
                  bool terrain_available, bool surface_model) const;
    };
} // namespace osect
