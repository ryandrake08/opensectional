#include "elevation_tile.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <sdl/surface.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace osect
{
    elevation_tile::elevation_tile(int width, int height, std::vector<float> elevations_m)
        : width_(width), height_(height), elevations_m_(std::move(elevations_m))
    {
    }

    elevation_tile elevation_tile::load(const std::filesystem::path& path)
    {
        const std::string filename = path.string();
        sdl::surface surface(filename.c_str());
        return from_rgba(surface.width(), surface.height(), static_cast<const uint8_t*>(surface.pixels()),
                         static_cast<size_t>(surface.width()) * 4);
    }

    elevation_tile elevation_tile::from_rgba(int width, int height, const uint8_t* rgba, size_t stride)
    {
        if(width <= 0 || height <= 0 || !rgba || stride < static_cast<size_t>(width) * 4)
        {
            throw std::invalid_argument("invalid Terrarium pixel buffer");
        }

        std::vector<float> elevations_m(static_cast<size_t>(width) * height);
        for(int y = 0; y < height; y++)
        {
            const uint8_t* row = rgba + static_cast<size_t>(y) * stride;
            for(int x = 0; x < width; x++)
            {
                const uint8_t* pixel = row + x * 4;
                const size_t index = static_cast<size_t>(y) * width + x;
                if(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0)
                {
                    elevations_m[index] = NAN;
                    continue;
                }
                elevations_m[index] = static_cast<float>(pixel[0] * 256 + pixel[1] + pixel[2] / 256.0 - 32768.0);
            }
        }
        return {width, height, std::move(elevations_m)};
    }

    int elevation_tile::width() const
    {
        return width_;
    }

    int elevation_tile::height() const
    {
        return height_;
    }

    float elevation_tile::sample_m(double x, double y) const
    {
        x = std::clamp(x, 0.0, static_cast<double>(width_ - 1));
        y = std::clamp(y, 0.0, static_cast<double>(height_ - 1));
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(x0 + 1, width_ - 1);
        const int y1 = std::min(y0 + 1, height_ - 1);
        const double tx = x - x0;
        const double ty = y - y0;

        const std::array<std::pair<float, double>, 4> samples = {
            std::make_pair(elevations_m_[static_cast<size_t>(y0) * width_ + x0], (1.0 - tx) * (1.0 - ty)),
            std::make_pair(elevations_m_[static_cast<size_t>(y0) * width_ + x1], tx * (1.0 - ty)),
            std::make_pair(elevations_m_[static_cast<size_t>(y1) * width_ + x0], (1.0 - tx) * ty),
            std::make_pair(elevations_m_[static_cast<size_t>(y1) * width_ + x1], tx * ty),
        };

        double elevation_m = 0.0;
        for(const auto& [sample, weight] : samples)
        {
            if(weight == 0.0)
            {
                continue;
            }
            if(std::isnan(sample))
            {
                return NAN;
            }
            elevation_m += sample * weight;
        }
        return static_cast<float>(elevation_m);
    }

    float elevation_tile::maximum_m(int x_min, int y_min, int x_max, int y_max) const
    {
        x_min = std::max(x_min, 0);
        y_min = std::max(y_min, 0);
        x_max = std::min(x_max, width_ - 1);
        y_max = std::min(y_max, height_ - 1);
        if(x_min > x_max || y_min > y_max)
        {
            return NAN;
        }

        float maximum = NAN;
        for(int y = y_min; y <= y_max; y++)
        {
            for(int x = x_min; x <= x_max; x++)
            {
                const float elevation_m = elevations_m_[static_cast<size_t>(y) * width_ + x];
                if(!std::isnan(elevation_m) && (std::isnan(maximum) || elevation_m > maximum))
                {
                    maximum = elevation_m;
                }
            }
        }
        return maximum;
    }

    std::vector<uint16_t> elevation_tile::quantized_m() const
    {
        std::vector<uint16_t> result;
        result.reserve(elevations_m_.size());
        for(const float elevation : elevations_m_)
        {
            if(!std::isfinite(elevation))
            {
                result.push_back(0);
                continue;
            }
            const auto metres = std::clamp(static_cast<int>(std::lround(elevation)), -999, 64535);
            result.push_back(static_cast<uint16_t>(metres + 1000));
        }
        return result;
    }
} // namespace osect
