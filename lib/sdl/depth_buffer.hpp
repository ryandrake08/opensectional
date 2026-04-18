#pragma once

#include "types.hpp"
#include <memory>

namespace sdl
{
    class device;
    class texture;

    /**
     * RAII wrapper for depth buffer texture.
     *
     * Manages a depth texture that matches window dimensions.
     * May re-create the texture when set_size() is called.
     * Non-copyable, moveable.
     */
    class depth_buffer
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Create depth buffer with specified dimensions.
         *
         * @param dev GPU device
         * @param width Width in pixels
         * @param height Height in pixels
         * @param format Depth texture format
         */
        depth_buffer(device& dev, unsigned width, unsigned height, texture_format_t format);

        ~depth_buffer();

        // Non-copyable
        depth_buffer(const depth_buffer&) = delete;
        depth_buffer& operator=(const depth_buffer&) = delete;

        // Moveable
        depth_buffer(depth_buffer&& other) noexcept;
        depth_buffer& operator=(depth_buffer&& other) noexcept;

        /**
         * Get the depth texture.
         *
         * @return Reference to depth texture
         */
        const texture& get() const;

        /**
         * Get the depth texture format.
         *
         * @return Depth texture format
         */
        texture_format_t format() const;

        /**
         * Sets the dimensions of depth texture.
         *
         * May re-create the depth texture to match new dimensions.
         *
         * @param width New width in pixels
         * @param height New height in pixels
         */
        void set_size(unsigned width, unsigned height);
    };

} // namespace sdl
