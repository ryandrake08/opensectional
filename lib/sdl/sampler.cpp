#include "sampler.hpp"
#include "device.hpp"
#include "error.hpp"
#include "types.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    // Sampler implementation
    struct sampler::impl
    {
        SDL_GPUDevice* device;  // Non-owning
        SDL_GPUSampler* handle; // Owning

        static SDL_GPUSampler* create_sampler(
            SDL_GPUDevice* dev,
            SDL_GPUFilter min_filter,
            SDL_GPUFilter mag_filter,
            SDL_GPUSamplerAddressMode address_mode)
        {
            SDL_GPUSamplerCreateInfo sampler_info = {};
            sampler_info.min_filter = min_filter;
            sampler_info.mag_filter = mag_filter;
            sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
            sampler_info.address_mode_u = address_mode;
            sampler_info.address_mode_v = address_mode;
            sampler_info.address_mode_w = address_mode;
            sampler_info.mip_lod_bias = 0.0F;
            sampler_info.max_anisotropy = 1.0F;
            sampler_info.compare_op = SDL_GPU_COMPAREOP_NEVER;
            sampler_info.min_lod = 0.0F;
            sampler_info.max_lod = 1000.0F;
            sampler_info.enable_anisotropy = false;
            sampler_info.enable_compare = false;

            return SDL_CreateGPUSampler(dev, &sampler_info);
        }

        impl(
            SDL_GPUDevice* dev,
            SDL_GPUFilter min_filter,
            SDL_GPUFilter mag_filter,
            SDL_GPUSamplerAddressMode address_mode) : device(dev), handle(create_sampler(dev, min_filter, mag_filter, address_mode))
        {
            if(!handle)
            {
                throw error("Failed to create GPU sampler");
            }
        }

        ~impl() noexcept
        {
            SDL_ReleaseGPUSampler(device, handle);
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    sampler::sampler(
        const device& dev,
        filter_t min_filter,
        filter_t mag_filter,
        sampler_address_mode_t address_mode) : pimpl(new impl(dev.get(),
                                                              static_cast<SDL_GPUFilter>(min_filter.value),
                                                              static_cast<SDL_GPUFilter>(mag_filter.value),
                                                              static_cast<SDL_GPUSamplerAddressMode>(address_mode.value)))
    {
    }

    sampler::~sampler() = default;

    sampler::sampler(sampler&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    sampler& sampler::operator=(sampler&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    SDL_GPUSampler* sampler::get() const
    {
        return pimpl->handle;
    }
} // namespace sdl
