#include "terrain_style.hpp"
#include "ini_config.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>

namespace osect
{
    namespace
    {
        [[noreturn]] void reject(const std::string& key, const std::string& constraint, const std::string& got)
        {
            throw std::runtime_error("terrain_style: " + key + " " + constraint + " (got " + got + ")");
        }

        template <typename T>
        void read_into(const ini_config& ini, const std::string& key, T& value)
        {
            if(ini.exists(key))
            {
                value = ini.get<T>(key);
            }
        }

        std::string trim(const std::string& s)
        {
            const auto a = s.find_first_not_of(" \t");
            if(a == std::string::npos)
            {
                return {};
            }
            return s.substr(a, s.find_last_not_of(" \t") - a + 1);
        }

        bool parse_hex_color(const std::string& text, float& r, float& g, float& b)
        {
            if(text.size() < 2 || text[0] != '#')
            {
                return false;
            }
            const std::string hex = text.substr(1);
            const auto nibble = [](char c) -> int
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
            };

            std::array<int, 3> channels{};
            if(hex.size() == 6)
            {
                size_t channel_index = 0;
                for(auto& channel : channels)
                {
                    const int hi = nibble(hex[channel_index * 2]);
                    const int lo = nibble(hex[channel_index * 2 + 1]);
                    if(hi < 0 || lo < 0)
                    {
                        return false;
                    }
                    channel = hi * 16 + lo;
                    ++channel_index;
                }
            }
            else if(hex.size() == 3)
            {
                size_t channel_index = 0;
                for(auto& channel : channels)
                {
                    const int v = nibble(hex[channel_index]);
                    if(v < 0)
                    {
                        return false;
                    }
                    channel = v * 17;
                    ++channel_index;
                }
            }
            else
            {
                return false;
            }

            r = static_cast<float>(channels[0]) / 255.0F;
            g = static_cast<float>(channels[1]) / 255.0F;
            b = static_cast<float>(channels[2]) / 255.0F;
            return true;
        }

        std::vector<hypsometric_stop> parse_ramp(const std::string& spec)
        {
            std::vector<hypsometric_stop> stops;
            std::stringstream stream(spec);
            std::string item;
            while(std::getline(stream, item, ','))
            {
                const std::string entry = trim(item);
                if(entry.empty())
                {
                    continue;
                }
                const auto colon = entry.find(':');
                if(colon == std::string::npos)
                {
                    reject("ramp", "entries must be 'elevation_m:#color'", entry);
                }
                hypsometric_stop stop{};
                try
                {
                    stop.elevation_m = std::stof(trim(entry.substr(0, colon)));
                }
                catch(const std::exception&)
                {
                    reject("ramp", "bad elevation", entry);
                }
                if(!parse_hex_color(trim(entry.substr(colon + 1)), stop.r, stop.g, stop.b))
                {
                    reject("ramp", "color must be #RRGGBB or #RGB", entry);
                }
                stops.push_back(stop);
            }
            return stops;
        }

        std::vector<hypsometric_stop> default_ramp()
        {
            const auto stop = [](float e, int r, int g, int b) -> hypsometric_stop
            { return {e, r / 255.0F, g / 255.0F, b / 255.0F}; };
            return {
                stop(0.0F, 96, 132, 92),      stop(675.0F, 150, 168, 110),  stop(1575.0F, 202, 190, 140),
                stop(2475.0F, 190, 150, 112), stop(3510.0F, 148, 122, 104), stop(4050.0F, 184, 176, 170),
                stop(4500.0F, 245, 245, 245),
            };
        }

        terrain_shading parse_mode(const std::string& text)
        {
            if(text == "hillshade")
            {
                return terrain_shading::hillshade;
            }
            if(text == "hypsometric")
            {
                return terrain_shading::hypsometric;
            }
            if(text == "cruise_relative")
            {
                return terrain_shading::cruise_relative;
            }
            reject("mode", "must be hillshade | hypsometric | cruise_relative", text);
        }
    } // namespace

    terrain_style::terrain_style(const ini_config& ini)
    {
        ramp = default_ramp();

        if(ini.exists("terrain.mode"))
        {
            mode = parse_mode(ini.get<std::string>("terrain.mode"));
        }
        read_into(ini, "terrain.opacity", opacity);
        read_into(ini, "terrain.sun_azimuth", sun_azimuth_deg);
        read_into(ini, "terrain.sun_altitude", sun_altitude_deg);
        read_into(ini, "terrain.exaggeration", vertical_exaggeration);
        read_into(ini, "terrain.cruise_warning", cruise_warning_ft);
        read_into(ini, "terrain.cruise_caution", cruise_caution_ft);
        read_into(ini, "terrain.cruise_clear", cruise_clear_ft);
        read_into(ini, "terrain.gpu_cache", gpu_tile_cache);
        if(ini.exists("terrain.ramp"))
        {
            ramp = parse_ramp(ini.get<std::string>("terrain.ramp"));
        }
        if(ini.exists("terrain.water_color"))
        {
            const std::string spec = trim(ini.get<std::string>("terrain.water_color"));
            if(!parse_hex_color(spec, water_r, water_g, water_b))
            {
                reject("water_color", "must be #RRGGBB or #RGB", spec);
            }
        }

        if(!(opacity >= 0.0F) || opacity > 1.0F)
        {
            reject("opacity", "must be in [0, 1]", std::to_string(opacity));
        }
        if(!(sun_azimuth_deg >= 0.0F) || sun_azimuth_deg > 360.0F)
        {
            reject("sun_azimuth", "must be in [0, 360]", std::to_string(sun_azimuth_deg));
        }
        if(!(sun_altitude_deg >= 0.0F) || sun_altitude_deg > 90.0F)
        {
            reject("sun_altitude", "must be in [0, 90]", std::to_string(sun_altitude_deg));
        }
        if(!std::isfinite(vertical_exaggeration) || vertical_exaggeration <= 0.0F)
        {
            reject("exaggeration", "must be > 0", std::to_string(vertical_exaggeration));
        }
        if(ramp.size() < 2)
        {
            reject("ramp", "needs at least two stops", std::to_string(ramp.size()));
        }
        for(std::size_t i = 1; i < ramp.size(); i++)
        {
            if(!(ramp[i].elevation_m > ramp[i - 1].elevation_m))
            {
                reject("ramp", "elevations must strictly increase", std::to_string(ramp[i].elevation_m));
            }
        }
        if(!(cruise_warning_ft >= 0.0F) || !(cruise_warning_ft < cruise_caution_ft) ||
           !(cruise_caution_ft < cruise_clear_ft))
        {
            reject("cruise bands", "need 0 <= warning < caution < clear",
                   std::to_string(cruise_warning_ft) + " / " + std::to_string(cruise_caution_ft) + " / " +
                       std::to_string(cruise_clear_ft));
        }
        if(gpu_tile_cache < 1)
        {
            reject("gpu_cache", "must be >= 1", std::to_string(gpu_tile_cache));
        }
    }
} // namespace osect
