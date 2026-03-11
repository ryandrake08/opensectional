#pragma once

#include "types.hpp"
#include <memory>

namespace sdl
{
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
        struct impl;
        std::unique_ptr<impl> pimpl;

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

        /**
         * Destroy texture.
         */
        ~texture();

        // Non-copyable
        texture(const texture&) = delete;
        texture& operator=(const texture&) = delete;

        // Moveable
        texture(texture&& other) noexcept;
        texture& operator=(texture&& other) noexcept;

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
