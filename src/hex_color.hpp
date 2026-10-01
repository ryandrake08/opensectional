#pragma once

#include <optional>
#include <string>

namespace osect
{
    // Channels in [0, 1]. `a` is 1 when the text has no alpha digits.
    struct hex_color
    {
        float r = 0.0F;
        float g = 0.0F;
        float b = 0.0F;
        float a = 1.0F;
        bool has_alpha = false;
    };

    // Parse #RGB, #RRGGBB, or #RRGGBBAA (case-insensitive digits).
    // Returns nullopt for any other text.
    std::optional<hex_color> parse_hex_color(const std::string& text);
}
