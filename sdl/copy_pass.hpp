#pragma once

#include "types.hpp"
#include <memory>
#include <vector>

namespace sdl
{
    class command_buffer;
    class device;
    class buffer;
    class surface;
    class texture;

    /**
     * RAII wrapper for SDL_GPUCopyPass
     *
     * Begins copy pass on construction, ends on destruction.
     * Manages transfer buffers internally - each upload creates its own
     * transfer buffer sized exactly to the data being uploaded.
     */
    class copy_pass
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Begin copy pass.
         *
         * @param cmd Command buffer
         * @throws std::runtime_error if copy pass creation fails
         */
        copy_pass(command_buffer& cmd);

        /**
         * End copy pass.
         */
        ~copy_pass();

        // Non-copyable
        copy_pass(const copy_pass&) = delete;
        copy_pass& operator=(const copy_pass&) = delete;

        // Moveable
        copy_pass(copy_pass&& other) noexcept;
        copy_pass& operator=(copy_pass&& other) noexcept;

        /**
         * Get underlying copy pass handle.
         *
         * @return Raw copy pass pointer (non-owning)
         */
        SDL_GPUCopyPass* get() const;

        /**
         * Create GPU buffer and upload data from vector.
         *
         * Creates a buffer and uploads data via an internally managed transfer buffer.
         *
         * @param dev GPU device
         * @param usage Buffer usage flags (e.g., SDL_GPU_BUFFERUSAGE_VERTEX, SDL_GPU_BUFFERUSAGE_INDEX)
         * @param data Vector of data to upload (must not be empty)
         * @return Created and populated buffer
         * @throws std::runtime_error if data is empty
         */
        template<typename T>
        buffer create_and_upload_buffer(const device& dev, buffer_usage_t usage, const std::vector<T>& data);

        /**
         * Create GPU texture and upload surface data.
         *
         * Creates a texture and uploads data via an internally managed transfer buffer.
         *
         * @param dev GPU device
         * @param surf Surface containing pixel data (RGBA8888 format)
         * @return Created and populated texture
         */
        texture create_and_upload_texture(const device& dev, const surface& surf);
    };

} // namespace sdl
