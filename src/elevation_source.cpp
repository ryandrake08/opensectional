#include "elevation_source.hpp"
#include "elevation_address.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
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
    elevation_source::elevation_source(std::filesystem::path path, size_t cache_capacity)
        : path_(std::move(path)), cache_capacity_(cache_capacity)
    {
        if(cache_capacity_ == 0)
        {
            throw std::invalid_argument("elevation tile cache capacity must be positive");
        }
        if(path_.empty())
        {
            return;
        }
        const std::filesystem::path manifest_path = path_ / "manifest.json";
        if(!std::filesystem::exists(manifest_path))
        {
            return;
        }

        const std::string manifest = read_file(manifest_path);
        (void)string_value(manifest, "dataset");
        display_name_ = string_value(manifest, "dataset_display_name");
        status_name_ = display_name_.substr(0, display_name_.find(" ("));
        source_version_ = string_value(manifest, "source_version");
        attribution_ = string_value(manifest, "attribution");
        surface_model_ = bool_value(manifest, "is_surface_model");
        if(string_value(manifest, "vertical_datum") != "EGM2008")
        {
            throw std::runtime_error("manifest.json: vertical_datum must be EGM2008");
        }
        vertical_precision_m_ = number_value(manifest, "vertical_precision_m");
        tile_size_ = integer_value(manifest, "tile_pixels");
        skirt_ = integer_value(manifest, "skirt_pixels");
        min_zoom_ = integer_value(manifest, "min_zoom");
        max_zoom_ = integer_value(manifest, "max_zoom");
        if(vertical_precision_m_ <= 0.0 || tile_size_ == 0 || max_zoom_ < min_zoom_ || max_zoom_ > 30)
        {
            throw std::runtime_error("manifest.json: invalid tile geometry");
        }

        // The optional water/ sidecar tree. Built over the same zoom
        // range as the height tiles; the zoom-range guard stays as a
        // defensive check on a hand-edited manifest.
        if(const auto water = object_value(manifest, "water_mask"))
        {
            has_water_mask_ = true;
            water_min_zoom_ = integer_value(*water, "min_zoom");
            water_max_zoom_ = integer_value(*water, "max_zoom");
            if(water_max_zoom_ < water_min_zoom_)
            {
                throw std::runtime_error("manifest.json: invalid water_mask zoom range");
            }
        }

        available_ = true;
    }

    bool elevation_source::available() const
    {
        return available_;
    }

    bool elevation_source::is_surface_model() const
    {
        return surface_model_;
    }

    int elevation_source::min_zoom() const
    {
        return min_zoom_;
    }

    int elevation_source::max_zoom() const
    {
        return max_zoom_;
    }

    int elevation_source::tile_size() const
    {
        return tile_size_;
    }

    int elevation_source::skirt() const
    {
        return skirt_;
    }

    bool elevation_source::has_water_mask() const
    {
        return has_water_mask_;
    }

    int elevation_source::water_min_zoom() const
    {
        return water_min_zoom_;
    }

    int elevation_source::water_max_zoom() const
    {
        return water_max_zoom_;
    }

    double elevation_source::vertical_precision_m() const
    {
        return vertical_precision_m_;
    }

    const std::filesystem::path& elevation_source::path() const
    {
        return path_;
    }

    const std::string& elevation_source::attribution() const
    {
        return attribution_;
    }

    data_source elevation_source::data_source_row() const
    {
        if(!available_)
        {
            return {"terrain", "Terrain unavailable", std::nullopt};
        }
        return {"terrain", status_name_ + " " + source_version_, std::nullopt};
    }

    std::shared_ptr<const elevation_tile> elevation_source::load_tile(const tile_key& key) const
    {
        std::unique_lock<std::mutex> lock(cache_mutex_);
        const auto cached = cache_.find(key);
        if(cached != cache_.end())
        {
            touch(cached);
            return cached->second.tile;
        }
        lock.unlock();

        const std::filesystem::path tile_path =
            path_ / std::to_string(key.z) / std::to_string(key.x) / (std::to_string(key.y) + ".png");
        std::shared_ptr<const elevation_tile> tile;
        if(std::filesystem::exists(tile_path))
        {
            tile = std::make_shared<elevation_tile>(elevation_tile::load(tile_path));
        }

        lock.lock();
        const auto inserted = cache_.emplace(key, cache_entry{tile, recency_.end()});
        if(!inserted.second)
        {
            touch(inserted.first);
            return inserted.first->second.tile;
        }
        recency_.push_front(key);
        inserted.first->second.recency = recency_.begin();
        if(cache_.size() > cache_capacity_)
        {
            const tile_key oldest = recency_.back();
            cache_.erase(oldest);
            recency_.pop_back();
        }
        return tile;
    }

    void elevation_source::touch(std::unordered_map<tile_key, cache_entry>::iterator entry) const
    {
        recency_.splice(recency_.begin(), recency_, entry->second.recency);
        entry->second.recency = recency_.begin();
    }

    std::optional<double> elevation_source::elevation_ft(double lat, double lon, int zoom) const
    {
        if(!available_ || zoom < min_zoom_ || zoom > max_zoom_)
        {
            return std::nullopt;
        }

        const elevation_address address = address_elevation(lat, lon, zoom, tile_size_, skirt_);
        const std::shared_ptr<const elevation_tile> tile = load_tile(address.key);
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
        if(!available_ || !std::isfinite(lat_min) || !std::isfinite(lon_min) || !std::isfinite(lat_max) ||
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
            const elevation_address address = address_elevation(lat, 0.0, zoom, tile_size_, skirt_);
            return address.key.y * tile_size_ + address.pixel_y - skirt_ + 0.5;
        };

        int zoom = max_zoom_;
        for(int candidate = min_zoom_; candidate <= max_zoom_; candidate++)
        {
            const double world_pixels = static_cast<double>(1 << candidate) * tile_size_;
            const double height_pixels = global_y(lat_min, candidate) - global_y(lat_max, candidate);
            if(longitude_width / 360.0 * world_pixels >= 1.0 - 1e-9 && height_pixels >= 1.0 - 1e-9)
            {
                zoom = candidate;
                break;
            }
        }

        const int tiles_per_axis = 1 << zoom;
        const double world_pixels = static_cast<double>(tiles_per_axis) * tile_size_;
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
            for(int64_t tile_y = first_y / tile_size_; tile_y <= last_y / tile_size_; tile_y++)
            {
                for(int64_t tile_x = first_x / tile_size_; tile_x <= last_x / tile_size_; tile_x++)
                {
                    const std::shared_ptr<const elevation_tile> tile =
                        load_tile({zoom, static_cast<int>(tile_x), static_cast<int>(tile_y)});
                    if(!tile)
                    {
                        continue;
                    }
                    const int x_min =
                        skirt_ + static_cast<int>(std::max(first_x, tile_x * tile_size_) - tile_x * tile_size_);
                    const int x_max = skirt_ + static_cast<int>(std::min(last_x, (tile_x + 1) * tile_size_ - 1) -
                                                                tile_x * tile_size_);
                    const int y_min =
                        skirt_ + static_cast<int>(std::max(first_y, tile_y * tile_size_) - tile_y * tile_size_);
                    const int y_max = skirt_ + static_cast<int>(std::min(last_y, (tile_y + 1) * tile_size_ - 1) -
                                                                tile_y * tile_size_);
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
