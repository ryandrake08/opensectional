#pragma once

#include "optional.hpp"
#include "types.hpp"
#include <memory>

namespace sdl
{
    class device;
    class window;
    class texture;

    /**
     * RAII wrapper for SDL_GPUCommandBuffer
     *
     * Acquires command buffer and automatically submits on destruction.
     * Non-copyable, non-moveable.
     */
    class command_buffer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Acquire command buffer from device.
         *
         * @param dev GPU device
         * @throws std::runtime_error if acquisition fails
         */
        explicit command_buffer(const device& dev);

        /**
         * Submit command buffer and destroy.
         */
        ~command_buffer();

        // Non-copyable
        command_buffer(const command_buffer&) = delete;
        command_buffer& operator=(const command_buffer&) = delete;

        // Moveable
        command_buffer(command_buffer&& other) noexcept;
        command_buffer& operator=(command_buffer&& other) noexcept;

        /**
         * Get underlying command buffer handle.
         */
        SDL_GPUCommandBuffer* get() const;

        /**
         * Acquire swapchain texture for rendering.
         *
         * @param win Window to acquire swapchain from
         * @param width Swapchain texture width
         * @param height Swapchain texture height
         * @return Non-owning texture wrapper (empty if window minimized/occluded)
         * @throws std::runtime_error if acquisition fails
         */
        optional<texture> acquire_swapchain(const window& win, unsigned& width, unsigned& height);
    };

} // namespace sdl
