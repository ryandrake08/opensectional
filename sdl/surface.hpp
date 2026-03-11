#pragma once

#include "types.hpp"
#include <memory>

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
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Load surface from image file.
         *
         * Uses SDL3_image to load common image formats (PNG, JPEG, BMP, etc.).
         * Automatically converts to RGBA8888 format for consistency.
         * Handles grayscale images by treating grayscale as alpha with white RGB.
         *
         * @param file_path Path to image file
         * @throws std::runtime_error if image loading or conversion fails
         */
        explicit surface(const char* file_path);

        /**
         * Destroy surface.
         */
        ~surface();

        // Non-copyable
        surface(const surface&) = delete;
        surface& operator=(const surface&) = delete;

        // Moveable
        surface(surface&& other) noexcept;
        surface& operator=(surface&& other) noexcept;

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
