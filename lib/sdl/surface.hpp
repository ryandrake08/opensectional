#pragma once

#include <cstdint>
#include <memory>

struct SDL_GPUTextureCreateInfo;
struct SDL_Surface;

namespace sdl
{
    /**
     * RAII wrapper for SDL_Surface.
     *
     * Manages CPU-side image loading and pixel data.
     * Non-copyable, moveable.
     */
    class surface
    {
        // Destroys the surface.
        struct release
        {
            void operator()(SDL_Surface* handle) const;
        };

        std::unique_ptr<SDL_Surface, release> handle_;

    public:
        /**
         * Load surface from image file.
         *
         * Uses SDL3_image to load common image formats (PNG, JPEG, BMP, etc.)
         * and converts the result to RGBA8888.
         *
         * SDL3 has no single-channel pixel format, so SDL3_image loads 8-bit
         * grayscale PNGs as INDEX8 with a synthetic identity palette,
         * indistinguishable on the SDL_Surface from a true paletted PNG.
         * When @p as_alpha_mask is true the loader treats indexed input as a
         * single-channel mask (white RGB, pixel value → alpha), which is what
         * UI/text atlases authored as grayscale PNGs want. When false (the
         * default) the loader expands paletted input through SDL_ConvertSurface,
         * producing opaque RGB suitable for color artwork.
         *
         * @param file_path Path to image file
         * @param as_alpha_mask Treat indexed/grayscale input as an alpha mask
         * @throws std::runtime_error if image loading or conversion fails
         */
        explicit surface(const char* file_path, bool as_alpha_mask = false);

        // Non-copyable
        surface(const surface&) = delete;
        surface& operator=(const surface&) = delete;

        // Moveable
        surface(surface&& other) noexcept = default;
        surface& operator=(surface&& other) noexcept = default;

        /**
         * Get surface width in pixels.
         *
         * @return Width in pixels
         */
        int width() const;

        /**
         * Get surface height in pixels.
         *
         * @return Height in pixels
         */
        int height() const;

        /**
         * Get total size of pixel data in bytes.
         *
         * For RGBA8888 format: width * height * 4 bytes.
         *
         * @return Size of pixel data in bytes
         */
        uint32_t size() const;

        /**
         * Get raw pixel data.
         *
         * Returns non-owning pointer to RGBA8888 pixel data.
         * Data layout is: width * height * 4 bytes (RGBA).
         *
         * @return Non-owning pointer to pixel data
         */
        const void* pixels() const;

        /**
         * Get underlying SDL_Surface handle.
         *
         * @return Raw surface pointer (non-owning)
         */
        SDL_Surface* get() const;

        /**
         * Get GPU texture creation info matching this surface's format.
         *
         * Provides SDL_GPUTextureCreateInfo with correct format, dimensions, and
         * standard settings suitable for uploading this surface to GPU.
         * The returned info is configured for:
         * - 2D texture type
         * - RGBA8888 format (R8G8B8A8_UNORM)
         * - Sampler usage
         * - Single mip level, single sample
         *
         * @return Texture creation info structure
         */
        SDL_GPUTextureCreateInfo texture_create_info() const;

        /**
         * Get image height from file without loading full image.
         *
         * Efficiently reads just the image dimensions using SDL_image.
         * Useful for querying image properties without full load overhead.
         *
         * @param file_path Path to image file
         * @return Image height in pixels
         * @throws std::runtime_error if image loading fails
         */
        static int get_image_height(const char* file_path);
    };

} // namespace sdl
