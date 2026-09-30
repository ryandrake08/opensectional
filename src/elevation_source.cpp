#include "elevation_source.hpp"
#include "elevation_address.hpp"
#include "elevation_tile.hpp"
#include "tile_key.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{
    std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream input(path);
        if(!input)
        {
            throw std::runtime_error("cannot read " + path.string());
        }
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    size_t value_offset(const std::string& json, const std::string& key)
    {
        const size_t key_offset = json.find('"' + key + '"');
        if(key_offset == std::string::npos)
        {
            throw std::runtime_error("manifest.json: missing " + key);
        }
        const size_t colon = json.find(':', key_offset + key.size() + 2);
        if(colon == std::string::npos)
        {
            throw std::runtime_error("manifest.json: invalid " + key);
        }
        return json.find_first_not_of(" \t\r\n", colon + 1);
    }

    // The code unit spelled by the four hex digits at json[offset], if they
    // are four hex digits.
    std::optional<uint32_t> hex_code_unit(const std::string& json, size_t offset)
    {
        if(offset + 4 > json.size())
        {
            return std::nullopt;
        }
        uint32_t value = 0;
        for(size_t i = offset; i < offset + 4; ++i)
        {
            const char c = json[i];
            uint32_t digit = 0;
            if(c >= '0' && c <= '9')
            {
                digit = c - '0';
            }
            else if(c >= 'a' && c <= 'f')
            {
                digit = c - 'a' + 10;
            }
            else if(c >= 'A' && c <= 'F')
            {
                digit = c - 'A' + 10;
            }
            else
            {
                return std::nullopt;
            }
            value = value * 16 + digit;
        }
        return value;
    }

    void append_utf8(std::string& out, uint32_t code_point)
    {
        if(code_point < 0x80)
        {
            out.push_back(static_cast<char>(code_point));
        }
        else if(code_point < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
        else if(code_point < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    std::string string_value(const std::string& json, const std::string& key)
    {
        size_t offset = value_offset(json, key);
        if(offset == std::string::npos || json[offset] != '"')
        {
            throw std::runtime_error("manifest.json: invalid " + key);
        }
        std::string value;
        for(++offset; offset < json.size(); ++offset)
        {
            const char character = json[offset];
            if(character == '"')
            {
                return value;
            }
            if(character != '\\')
            {
                value.push_back(character);
                continue;
            }
            if(++offset == json.size())
            {
                break;
            }
            switch(json[offset])
            {
            case '"':
            case '\\':
            case '/':
                value.push_back(json[offset]);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'u':
            {
                // A code point above U+FFFF is a high surrogate escape
                // followed by a low surrogate escape.
                auto code = hex_code_unit(json, offset + 1);
                if(!code || (*code >= 0xDC00 && *code <= 0xDFFF))
                {
                    throw std::runtime_error("manifest.json: invalid " + key);
                }
                offset += 4;
                if(*code >= 0xD800 && *code <= 0xDBFF)
                {
                    const auto low =
                        json.compare(offset + 1, 2, "\\u") == 0 ? hex_code_unit(json, offset + 3) : std::nullopt;
                    if(!low || *low < 0xDC00 || *low > 0xDFFF)
                    {
                        throw std::runtime_error("manifest.json: invalid " + key);
                    }
                    code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                    offset += 6;
                }
                append_utf8(value, *code);
                break;
            }
            default:
                throw std::runtime_error("manifest.json: invalid " + key);
            }
        }
        throw std::runtime_error("manifest.json: invalid " + key);
    }

    double number_value(const std::string& json, const std::string& key)
    {
        const size_t offset = value_offset(json, key);
        try
        {
            size_t used = 0;
            const double value = std::stod(json.substr(offset), &used);
            if(used == 0 || !std::isfinite(value))
            {
                throw std::runtime_error("manifest.json: invalid " + key);
            }
            return value;
        }
        catch(const std::invalid_argument&)
        {
            throw std::runtime_error("manifest.json: invalid " + key);
        }
        catch(const std::out_of_range&)
        {
            throw std::runtime_error("manifest.json: invalid " + key);
        }
    }

    int integer_value(const std::string& json, const std::string& key)
    {
        const double value = number_value(json, key);
        if(std::floor(value) != value || value < 0 || value > 2147483647.0)
        {
            throw std::runtime_error("manifest.json: invalid " + key);
        }
        return static_cast<int>(value);
    }

    bool bool_value(const std::string& json, const std::string& key)
    {
        const size_t offset = value_offset(json, key);
        if(json.compare(offset, 4, "true") == 0)
        {
            return true;
        }
        if(json.compare(offset, 5, "false") == 0)
        {
            return false;
        }
        throw std::runtime_error("manifest.json: invalid " + key);
    }

    // The brace-delimited object that `key` maps to, or nullopt when the
    // key is absent. Used to scope nested lookups (e.g. water_mask's own
    // min_zoom, which must not resolve to the top-level one).
    std::optional<std::string> object_value(const std::string& json, const std::string& key)
    {
        const size_t k = json.find('"' + key + '"');
        if(k == std::string::npos)
        {
            return std::nullopt;
        }
        const size_t open = json.find('{', k);
        if(open == std::string::npos)
        {
            throw std::runtime_error("manifest.json: invalid " + key);
        }
        int depth = 0;
        for(size_t i = open; i < json.size(); ++i)
        {
            if(json[i] == '{')
            {
                ++depth;
            }
            else if(json[i] == '}' && --depth == 0)
            {
                return json.substr(open, i - open + 1);
            }
        }
        throw std::runtime_error("manifest.json: invalid " + key);
    }
}

namespace osect
{
    struct elevation_source::impl
    {
        std::filesystem::path path;
        bool available = false;
        int min_zoom = 0;
        int max_zoom = 0;
        int tile_size = 0;
        int skirt = 0;
        bool has_water_mask = false;
        int water_min_zoom = 0;
        int water_max_zoom = 0;
        double vertical_precision_m = 0.0;
        std::string attribution;
        std::string display_name;
        std::string status_name;
        std::string source_version;
        bool surface_model = false;
        size_t cache_capacity;

        struct cache_entry
        {
            std::shared_ptr<const elevation_tile> tile;
            std::list<tile_key>::iterator recency;
        };
        std::mutex cache_mutex;
        std::unordered_map<tile_key, cache_entry> cache;
        std::list<tile_key> recency;

        impl(std::filesystem::path path, size_t cache_capacity) : path(std::move(path)), cache_capacity(cache_capacity)
        {
        }

        // Loads a tile on demand and retains it in the LRU cache.
        std::shared_ptr<const elevation_tile> load_tile(const tile_key& key)
        {
            std::unique_lock<std::mutex> lock(cache_mutex);
            const auto cached = cache.find(key);
            if(cached != cache.end())
            {
                touch(cached);
                return cached->second.tile;
            }
            lock.unlock();

            const std::filesystem::path tile_path =
                path / std::to_string(key.z) / std::to_string(key.x) / (std::to_string(key.y) + ".png");
            std::shared_ptr<const elevation_tile> tile;
            if(std::filesystem::exists(tile_path))
            {
                tile = std::make_shared<elevation_tile>(elevation_tile::load(tile_path));
            }

            lock.lock();
            const auto inserted = cache.emplace(key, cache_entry{tile, recency.end()});
            if(!inserted.second)
            {
                touch(inserted.first);
                return inserted.first->second.tile;
            }
            recency.push_front(key);
            inserted.first->second.recency = recency.begin();
            if(cache.size() > cache_capacity)
            {
                const tile_key oldest = recency.back();
                cache.erase(oldest);
                recency.pop_back();
            }
            return tile;
        }

        void touch(std::unordered_map<tile_key, cache_entry>::iterator entry)
        {
            recency.splice(recency.begin(), recency, entry->second.recency);
            entry->second.recency = recency.begin();
        }
    };

    elevation_source::elevation_source(std::filesystem::path path, size_t cache_capacity)
        : pimpl(std::make_unique<impl>(std::move(path), cache_capacity))
    {
        if(pimpl->cache_capacity == 0)
        {
            throw std::invalid_argument("elevation tile cache capacity must be positive");
        }
        if(pimpl->path.empty())
        {
            return;
        }
        const std::filesystem::path manifest_path = pimpl->path / "manifest.json";
        if(!std::filesystem::exists(manifest_path))
        {
            return;
        }

        const std::string manifest = read_file(manifest_path);
        (void)string_value(manifest, "dataset");
        pimpl->display_name = string_value(manifest, "dataset_display_name");
        pimpl->status_name = pimpl->display_name.substr(0, pimpl->display_name.find(" ("));
        pimpl->source_version = string_value(manifest, "source_version");
        pimpl->attribution = string_value(manifest, "attribution");
        pimpl->surface_model = bool_value(manifest, "is_surface_model");
        if(string_value(manifest, "vertical_datum") != "EGM2008")
        {
            throw std::runtime_error("manifest.json: vertical_datum must be EGM2008");
        }
        pimpl->vertical_precision_m = number_value(manifest, "vertical_precision_m");
        pimpl->tile_size = integer_value(manifest, "tile_pixels");
        pimpl->skirt = integer_value(manifest, "skirt_pixels");
        pimpl->min_zoom = integer_value(manifest, "min_zoom");
        pimpl->max_zoom = integer_value(manifest, "max_zoom");
        if(pimpl->vertical_precision_m <= 0.0 || pimpl->tile_size == 0 || pimpl->max_zoom < pimpl->min_zoom ||
           pimpl->max_zoom > 30)
        {
            throw std::runtime_error("manifest.json: invalid tile geometry");
        }

        // The optional water/ sidecar tree. Built over the same zoom
        // range as the height tiles; the zoom-range guard stays as a
        // defensive check on a hand-edited manifest.
        if(const auto water = object_value(manifest, "water_mask"))
        {
            pimpl->has_water_mask = true;
            pimpl->water_min_zoom = integer_value(*water, "min_zoom");
            pimpl->water_max_zoom = integer_value(*water, "max_zoom");
            if(pimpl->water_max_zoom < pimpl->water_min_zoom)
            {
                throw std::runtime_error("manifest.json: invalid water_mask zoom range");
            }
        }

        pimpl->available = true;
    }

    elevation_source::~elevation_source() = default;

    bool elevation_source::available() const
    {
        return pimpl->available;
    }

    bool elevation_source::is_surface_model() const
    {
        return pimpl->surface_model;
    }

    int elevation_source::min_zoom() const
    {
        return pimpl->min_zoom;
    }

    int elevation_source::max_zoom() const
    {
        return pimpl->max_zoom;
    }

    int elevation_source::tile_size() const
    {
        return pimpl->tile_size;
    }

    int elevation_source::skirt() const
    {
        return pimpl->skirt;
    }

    bool elevation_source::has_water_mask() const
    {
        return pimpl->has_water_mask;
    }

    int elevation_source::water_min_zoom() const
    {
        return pimpl->water_min_zoom;
    }

    int elevation_source::water_max_zoom() const
    {
        return pimpl->water_max_zoom;
    }

    double elevation_source::vertical_precision_m() const
    {
        return pimpl->vertical_precision_m;
    }

    const std::filesystem::path& elevation_source::path() const
    {
        return pimpl->path;
    }

    const std::string& elevation_source::attribution() const
    {
        return pimpl->attribution;
    }

    data_source elevation_source::data_source_row() const
    {
        if(!pimpl->available)
        {
            return {"terrain", "Terrain unavailable", std::nullopt};
        }
        return {"terrain", pimpl->status_name + " " + pimpl->source_version, std::nullopt};
    }

    std::optional<double> elevation_source::elevation_ft(double lat, double lon, int zoom) const
    {
        if(!pimpl->available || zoom < pimpl->min_zoom || zoom > pimpl->max_zoom)
        {
            return std::nullopt;
        }

        const elevation_address address = address_elevation(lat, lon, zoom, pimpl->tile_size, pimpl->skirt);
        const std::shared_ptr<const elevation_tile> tile = pimpl->load_tile(address.key);
        if(!tile)
        {
            return std::nullopt;
        }
        const float elevation_m = tile->sample_m(address.pixel_x, address.pixel_y);
        if(std::isnan(elevation_m))
        {
            return std::nullopt;
        }
        return elevation_m * 3.280839895013123;
    }

    std::optional<double> elevation_source::maximum_elevation_ft(double lat_min, double lon_min, double lat_max,
                                                                 double lon_max) const
    {
        if(!pimpl->available || !std::isfinite(lat_min) || !std::isfinite(lon_min) || !std::isfinite(lat_max) ||
           !std::isfinite(lon_max) || lat_min >= lat_max)
        {
            return std::nullopt;
        }

        double longitude_width = lon_max - lon_min;
        if(std::abs(longitude_width) >= 360.0)
        {
            longitude_width = 360.0;
        }
        else
        {
            longitude_width = std::fmod(longitude_width, 360.0);
            if(longitude_width < 0.0)
            {
                longitude_width += 360.0;
            }
        }
        if(longitude_width == 0.0)
        {
            return std::nullopt;
        }

        const auto global_y = [this](double lat, int zoom)
        {
            const elevation_address address = address_elevation(lat, 0.0, zoom, pimpl->tile_size, pimpl->skirt);
            return address.key.y * pimpl->tile_size + address.pixel_y - pimpl->skirt + 0.5;
        };

        int zoom = pimpl->max_zoom;
        for(int candidate = pimpl->min_zoom; candidate <= pimpl->max_zoom; candidate++)
        {
            const double world_pixels = static_cast<double>(1 << candidate) * pimpl->tile_size;
            const double height_pixels = global_y(lat_min, candidate) - global_y(lat_max, candidate);
            if(longitude_width / 360.0 * world_pixels >= 1.0 - 1e-9 && height_pixels >= 1.0 - 1e-9)
            {
                zoom = candidate;
                break;
            }
        }

        const int tiles_per_axis = 1 << zoom;
        const double world_pixels = static_cast<double>(tiles_per_axis) * pimpl->tile_size;
        double longitude_start = std::fmod(lon_min + 180.0, 360.0);
        if(longitude_start < 0.0)
        {
            longitude_start += 360.0;
        }
        longitude_start = longitude_start / 360.0 * world_pixels;
        const double longitude_end = longitude_start + longitude_width / 360.0 * world_pixels;
        const double y_start = global_y(lat_max, zoom);
        const double y_end = global_y(lat_min, zoom);
        if(y_start >= y_end)
        {
            return std::nullopt;
        }

        float maximum_m = NAN;
        const auto scan = [this, zoom, y_start, y_end, &maximum_m](double x_start, double x_end)
        {
            const auto first_x = static_cast<int64_t>(std::floor(x_start));
            const auto last_x = static_cast<int64_t>(std::ceil(x_end)) - 1;
            const auto first_y = static_cast<int64_t>(std::floor(y_start));
            const auto last_y = static_cast<int64_t>(std::ceil(y_end)) - 1;
            for(int64_t tile_y = first_y / pimpl->tile_size; tile_y <= last_y / pimpl->tile_size; tile_y++)
            {
                for(int64_t tile_x = first_x / pimpl->tile_size; tile_x <= last_x / pimpl->tile_size; tile_x++)
                {
                    const std::shared_ptr<const elevation_tile> tile =
                        pimpl->load_tile({zoom, static_cast<int>(tile_x), static_cast<int>(tile_y)});
                    if(!tile)
                    {
                        continue;
                    }
                    const int x_min = pimpl->skirt + static_cast<int>(std::max(first_x, tile_x * pimpl->tile_size) -
                                                                      tile_x * pimpl->tile_size);
                    const int x_max =
                        pimpl->skirt + static_cast<int>(std::min(last_x, (tile_x + 1) * pimpl->tile_size - 1) -
                                                        tile_x * pimpl->tile_size);
                    const int y_min = pimpl->skirt + static_cast<int>(std::max(first_y, tile_y * pimpl->tile_size) -
                                                                      tile_y * pimpl->tile_size);
                    const int y_max =
                        pimpl->skirt + static_cast<int>(std::min(last_y, (tile_y + 1) * pimpl->tile_size - 1) -
                                                        tile_y * pimpl->tile_size);
                    const float tile_maximum = tile->maximum_m(x_min, y_min, x_max, y_max);
                    if(!std::isnan(tile_maximum) && (std::isnan(maximum_m) || tile_maximum > maximum_m))
                    {
                        maximum_m = tile_maximum;
                    }
                }
            }
        };

        scan(longitude_start, std::min(longitude_end, world_pixels));
        if(longitude_end > world_pixels)
        {
            scan(0.0, longitude_end - world_pixels);
        }
        if(std::isnan(maximum_m))
        {
            return std::nullopt;
        }
        return maximum_m * 3.280839895013123;
    }
} // namespace osect
