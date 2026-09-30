#pragma once

#include "opaque_typedef.hpp"
#include <cstdint>
#include <memory>

struct SDL_GPUDevice;
struct SDL_GPUSampler;

namespace sdl
{
    // ========================================================================
    // GPU Texture Filtering (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, filter_t);

    namespace filter
    {
        extern const filter_t nearest;
        extern const filter_t linear;
    }

    // ========================================================================
    // GPU Sampler Address Mode (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, sampler_address_mode_t);

    namespace sampler_address_mode
    {
        extern const sampler_address_mode_t repeat;
        extern const sampler_address_mode_t mirrored_repeat;
        extern const sampler_address_mode_t clamp_to_edge;
    }

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
        // Releases the handle on the device that created it.
        struct release
        {
            SDL_GPUDevice* device;
            void operator()(SDL_GPUSampler* handle) const;
        };

        std::unique_ptr<SDL_GPUSampler, release> handle_;

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
        sampler(const device& dev, filter_t min_filter = filter::linear, filter_t mag_filter = filter::linear,
                sampler_address_mode_t address_mode = sampler_address_mode::clamp_to_edge);

        // Non-copyable
        sampler(const sampler&) = delete;
        sampler& operator=(const sampler&) = delete;

        // Moveable
        sampler(sampler&& other) noexcept = default;
        sampler& operator=(sampler&& other) noexcept = default;

        /**
         * Get underlying SDL_GPUSampler handle.
         *
         * @return Raw sampler pointer (non-owning)
         */
        SDL_GPUSampler* get() const;
    };

} // namespace sdl
