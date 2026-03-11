#pragma once

#include "types.hpp"
#include <memory>

namespace sdl
{
    class device;

    /**
     * RAII wrapper for SDL_GPUBuffer
     *
     * Manages GPU buffer creation, updates, and cleanup.
     * Non-copyable, moveable.
     */
    class buffer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Create GPU buffer.
         *
         * @param dev GPU device
         * @param usage Buffer usage flags
         * @param num Number of objects
         * @param size Size of each object
         * @throws std::runtime_error if buffer creation fails
         */
        buffer(const device& dev, buffer_usage_t usage, uint32_t num, uint32_t size);

        /**
         * Destroy buffer.
         */
        ~buffer();

        // Non-copyable
        buffer(const buffer&) = delete;
        buffer& operator=(const buffer&) = delete;

        // Moveable
        buffer(buffer&& other) noexcept;
        buffer& operator=(buffer&& other) noexcept;

        /**
         * Get underlying SDL_GPUBuffer handle.
         *
         * @return Raw buffer pointer (non-owning)
         */
        SDL_GPUBuffer* get() const;

        /**
         * Get buffer size in number of objects (vertices or indices).
         */
        uint32_t count() const;
    };

} // namespace sdl
