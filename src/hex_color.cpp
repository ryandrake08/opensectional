#include "hex_color.hpp"
#include <array>
#include <cstddef>

namespace osect
{
    namespace
    {
        int hex_digit(char c)
        {
            if(c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if(c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if(c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        }
    }

    std::optional<hex_color> parse_hex_color(const std::string& text)
    {
        if(text.empty() || text[0] != '#')
        {
            return std::nullopt;
        }
        const std::size_t digits = text.size() - 1;
        if(digits != 3 && digits != 6 && digits != 8)
        {
            return std::nullopt;
        }

        // #RGB has one digit per channel, repeated (0xA -> 0xAA); the
        // longer forms have two.
        const std::size_t width = digits == 3 ? 1 : 2;
        std::array<float, 4> channels{0.0F, 0.0F, 0.0F, 1.0F};
        for(std::size_t i = 0; i < digits / width; ++i)
        {
            int value = 0;
            for(std::size_t j = 0; j < width; ++j)
            {
                const int d = hex_digit(text[1 + i * width + j]);
                if(d < 0)
                {
                    return std::nullopt;
                }
                value = value * 16 + d;
            }
            if(width == 1)
            {
                value *= 17;
            }
            channels[i] = static_cast<float>(value) / 255.0F;
        }
        return hex_color{channels[0], channels[1], channels[2], channels[3], digits == 8};
    }
}
