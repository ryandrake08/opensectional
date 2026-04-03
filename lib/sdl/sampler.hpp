#pragma once

#include "types.hpp"
#include <memory>

namespace sdl
{
    class device;

    /**
     * RAII wrapper for SDL_GPUSampler
     *
     * Manages GPU sampler creation and cleanup.
     * Samplers control how textures are filtered and wrapped.
     * Non-copyable, moveable.
     */
    class sampler
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Create GPU sampler.
         *
         * @param dev GPU device
         * @param min_filter Minification filter (default: LINEAR)
         * @param mag_filter Magnification filter (default: LINEAR)
         * @param address_mode Address mode for UVW (default: CLAMP_TO_EDGE)
         * @throws std::runtime_error if sampler creation fails
         */
        sampler(
            const device& dev,
            filter_t min_filter = filter::linear,
            filter_t mag_filter = filter::linear,
            sampler_address_mode_t address_mode = sampler_address_mode::clamp_to_edge);

        /**
         * Destroy sampler.
         */
        ~sampler();

        // Non-copyable
        sampler(const sampler&) = delete;
        sampler& operator=(const sampler&) = delete;

        // Moveable
        sampler(sampler&& other) noexcept;
        sampler& operator=(sampler&& other) noexcept;

        /**
         * Get underlying SDL_GPUSampler handle.
         *
         * @return Raw sampler pointer (non-owning)
         */
        SDL_GPUSampler* get() const;
    };

} // namespace sdl
