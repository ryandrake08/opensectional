#pragma once

#include "opaque_typedef.hpp"
#include <cstdint>
#include <memory>

struct SDL_GPUDevice;
struct SDL_GPUTexture;

namespace sdl
{
    // ========================================================================
    // GPU Texture Format (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, texture_format_t);

    namespace texture_format
    {
        extern const texture_format_t r8_unorm;
        extern const texture_format_t r8g8_unorm;
        extern const texture_format_t r8g8b8a8_unorm;
        extern const texture_format_t r16_unorm;
        extern const texture_format_t r16g16_unorm;
        extern const texture_format_t r16g16b16a16_unorm;
        extern const texture_format_t r10g10b10a2_unorm;
        extern const texture_format_t r8g8b8a8_unorm_srgb;
        extern const texture_format_t d16_unorm;
        extern const texture_format_t d24_unorm;
        extern const texture_format_t d32_float;
        extern const texture_format_t d24_unorm_s8_uint;
        extern const texture_format_t d32_float_s8_uint;
    }

    class device;
    class surface;
    class copy_pass;

    /**
     * RAII wrapper for SDL_GPUTexture
     *
     * Manages GPU texture creation and cleanup.
     * Non-copyable, moveable.
     */
    class texture
    {
        // Releases the handle on its device. A wrapped (non-owning) texture
        // has a null device and is left to its owner.
        struct release
        {
            SDL_GPUDevice* device;
            void operator()(SDL_GPUTexture* handle) const;
        };

        std::unique_ptr<SDL_GPUTexture, release> handle_;

    public:
        /**
         * Create GPU texture from surface dimensions.
         *
         * Creates a GPU texture with dimensions matching the surface.
         * Texture is created but not uploaded - use with copy_pass for upload.
         *
         * @param dev GPU device
         * @param surf Surface to match dimensions
         * @throws std::runtime_error if texture creation fails
         */
        texture(const device& dev, const surface& surf);

        /**
         * Create a sampled 2D texture with an explicit format and size.
         *
         * The texture is created but not uploaded; use copy_pass for upload.
         */
        texture(const device& dev, unsigned width, unsigned height, texture_format_t format);

        /**
         * Wrap existing GPU texture (non-owning).
         *
         * Creates a non-owning wrapper around an existing SDL_GPUTexture.
         * The texture will NOT be released on destruction.
         * Useful for wrapping textures owned by other systems (e.g., SDL3_ttf atlas).
         *
         * @param raw_texture Existing texture to wrap (must remain valid)
         */
        explicit texture(SDL_GPUTexture* raw_texture);

        /**
         * Take ownership of existing GPU texture.
         *
         * Creates an owning wrapper around an existing SDL_GPUTexture.
         * The texture WILL be released on destruction.
         *
         * @param dev Raw GPU device pointer (for releasing texture)
         * @param raw_texture Existing texture to take ownership of
         */
        texture(SDL_GPUDevice* dev, SDL_GPUTexture* raw_texture);

        // Non-copyable
        texture(const texture&) = delete;
        texture& operator=(const texture&) = delete;

        // Moveable
        texture(texture&& other) noexcept = default;
        texture& operator=(texture&& other) noexcept = default;

        /**
         * Get underlying SDL_GPUTexture handle.
         *
         * @return Raw texture pointer (non-owning)
         */
        SDL_GPUTexture* get() const;

        /**
         * Get image height from file without creating a GPU texture.
         *
         * Efficiently reads just the image dimensions using SDL_image.
         * Useful for querying image properties without GPU upload overhead.
         *
         * @param file_path Path to image file
         * @return Image height in pixels
         * @throws std::runtime_error if image loading fails
         */
        static int get_image_height(const char* file_path);
    };

} // namespace sdl
