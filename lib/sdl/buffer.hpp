#pragma once

#include "bitflags.hpp"
#include <cstdint>
#include <memory>

struct SDL_GPUBuffer;
struct SDL_GPUDevice;

namespace sdl
{
    // ========================================================================
    // GPU Buffer Usage Flags (bitflags)
    // ========================================================================

    bitflags_typedef(uint32_t, buffer_usage_t);

    namespace buffer_usage
    {
        extern const buffer_usage_t vertex;
        extern const buffer_usage_t index;
        extern const buffer_usage_t indirect;
        extern const buffer_usage_t graphics_storage_read;
        extern const buffer_usage_t compute_storage_read;
        extern const buffer_usage_t compute_storage_write;
    }

    class device;

    /**
     * RAII wrapper for SDL_GPUBuffer
     *
     * Manages GPU buffer creation, updates, and cleanup.
     * Non-copyable, moveable.
     */
    class buffer
    {
        // Releases the handle on the device that created it.
        struct release
        {
            SDL_GPUDevice* device;
            void operator()(SDL_GPUBuffer* handle) const;
        };

        std::unique_ptr<SDL_GPUBuffer, release> handle_;
        uint32_t count_;
        uint32_t byte_size_;

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

        // Non-copyable
        buffer(const buffer&) = delete;
        buffer& operator=(const buffer&) = delete;

        // Moveable
        buffer(buffer&& other) noexcept = default;
        buffer& operator=(buffer&& other) noexcept = default;

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

        /**
         * Get buffer size in bytes (num * size from construction).
         */
        uint32_t byte_size() const;
    };

} // namespace sdl
