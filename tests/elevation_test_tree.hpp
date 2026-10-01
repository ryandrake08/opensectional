#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace osect::test
{
    // A terrain tree directory holding manifest.json, removed on destruction.
    class temporary_tree
    {
        std::filesystem::path path_;

    public:
        // Creates a new directory named <temp>/<name>-<n> for the first n
        // not already taken; create_directory fails when the name exists, so
        // concurrent runs never share a directory.
        temporary_tree(const std::string& name, const std::string& contents)
        {
            const auto base = std::filesystem::temp_directory_path() / (name + "-");
            for(int i = 0; i < 1000 && path_.empty(); ++i)
            {
                const std::filesystem::path candidate = base.string() + std::to_string(i);
                if(std::filesystem::create_directory(candidate))
                {
                    path_ = candidate;
                }
            }
            if(path_.empty())
            {
                throw std::runtime_error("could not create a temporary terrain tree");
            }
            std::ofstream(path_ / "manifest.json") << contents;
        }

        ~temporary_tree()
        {
            std::error_code ec;
            std::filesystem::remove_all(path_, ec);
        }

        temporary_tree(const temporary_tree&) = delete;
        temporary_tree& operator=(const temporary_tree&) = delete;

        const std::filesystem::path& path() const
        {
            return path_;
        }
    };

    inline std::string manifest(const std::string& dataset, int tile_pixels, int skirt_pixels, int min_zoom,
                                int max_zoom, double precision, const std::string& datum = "EGM2008",
                                const std::string& water_mask = {}, bool surface_model = false,
                                const std::string& bbox = "[-180, -85.0511287798066, 180, 85.0511287798066]")
    {
        return "{\n"
               "  \"dataset\": \"" + dataset + "\",\n"
               "  \"dataset_display_name\": \"" + dataset + " display\",\n"
               "  \"source_version\": \"2026-01-01\",\n"
               "  \"attribution\": \"Test data\",\n"
               "  \"is_surface_model\": " + std::string(surface_model ? "true" : "false") + ",\n"
               "  \"vertical_datum\": \"" + datum + "\",\n"
               "  \"vertical_precision_m\": " + std::to_string(precision) + ",\n"
               "  \"bbox\": " + bbox + ",\n"
               "  \"tile_pixels\": " + std::to_string(tile_pixels) + ",\n"
               "  \"skirt_pixels\": " + std::to_string(skirt_pixels) + ",\n"
               "  \"min_zoom\": " + std::to_string(min_zoom) + ",\n"
               "  \"max_zoom\": " + std::to_string(max_zoom) +
               (water_mask.empty() ? "" : ",\n  \"water_mask\": " + water_mask) + "\n"
               "}\n";
    }

    // Writes a 1x1 Terrarium tile of 0 m, or of 100 m when `high`.
    inline void write_elevation_tile(const std::filesystem::path& path, bool high)
    {
        static constexpr uint8_t zero[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1,
            8, 6, 0, 0, 0, 31, 21, 196, 137, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 104, 96, 96,
            248, 15, 0, 3, 4, 1, 128, 11, 131, 200, 20, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130,
        };
        static constexpr uint8_t hundred[] = {
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1,
            8, 6, 0, 0, 0, 31, 21, 196, 137, 0, 0, 0, 13, 73, 68, 65, 84, 120, 156, 99, 104, 72, 97,
            248, 15, 0, 4, 48, 1, 228, 10, 106, 56, 133, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130,
        };
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        const uint8_t* png = high ? hundred : zero;
        output.write(reinterpret_cast<const char*>(png), sizeof(zero));
    }

    // Writes a 1x1 Terrarium tile of `elevation_m` (to 1/256 m). The PNG
    // holds one RGBA scanline in an uncompressed deflate block.
    inline void write_elevation_tile_m(const std::filesystem::path& path, double elevation_m)
    {
        const auto crc32 = [](const std::string& bytes)
        {
            uint32_t crc = 0xFFFFFFFFU;
            for(const char c : bytes)
            {
                crc ^= static_cast<uint8_t>(c);
                for(int bit = 0; bit < 8; ++bit)
                {
                    crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
                }
            }
            return crc ^ 0xFFFFFFFFU;
        };
        const auto big_endian = [](uint32_t value)
        {
            return std::string{static_cast<char>(value >> 24U), static_cast<char>(value >> 16U),
                               static_cast<char>(value >> 8U), static_cast<char>(value)};
        };
        const auto chunk = [&](const std::string& type, const std::string& data)
        { return big_endian(static_cast<uint32_t>(data.size())) + type + data + big_endian(crc32(type + data)); };

        const double encoded = elevation_m + 32768.0;
        const auto whole = static_cast<uint32_t>(std::floor(encoded));
        const auto fraction = static_cast<uint32_t>(std::lround((encoded - std::floor(encoded)) * 256.0));
        const std::string scanline{'\0', static_cast<char>(whole >> 8U), static_cast<char>(whole & 0xFFU),
                                   static_cast<char>(fraction), static_cast<char>(0xFF)};

        uint32_t a = 1;
        uint32_t b = 0;
        for(const char c : scanline)
        {
            a = (a + static_cast<uint8_t>(c)) % 65521U;
            b = (b + a) % 65521U;
        }
        const auto length = static_cast<uint16_t>(scanline.size());
        const std::string zlib = std::string{'\x78', '\x01', '\x01'} + static_cast<char>(length & 0xFFU) +
                                 static_cast<char>(length >> 8U) + static_cast<char>(~length & 0xFFU) +
                                 static_cast<char>((~length >> 8U) & 0xFFU) + scanline + big_endian((b << 16U) | a);
        const std::string header{'\0', '\0', '\0', '\x01', '\0', '\0', '\0', '\x01', '\x08', '\x06', '\0', '\0', '\0'};

        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << std::string{"\x89PNG\r\n\x1a\n", 8} << chunk("IHDR", header) << chunk("IDAT", zlib)
               << chunk("IEND", "");
    }
}
