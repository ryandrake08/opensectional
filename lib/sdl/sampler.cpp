#include "sampler.hpp"
#include "device.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace filter
    {
        const filter_t nearest(SDL_GPU_FILTER_NEAREST);
        const filter_t linear(SDL_GPU_FILTER_LINEAR);
    }

    namespace sampler_address_mode
    {
        const sampler_address_mode_t repeat(SDL_GPU_SAMPLERADDRESSMODE_REPEAT);
        const sampler_address_mode_t mirrored_repeat(SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT);
        const sampler_address_mode_t clamp_to_edge(SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE);
    }

    namespace
    {
        SDL_GPUSampler* create_sampler(SDL_GPUDevice* dev, SDL_GPUFilter min_filter, SDL_GPUFilter mag_filter,
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

            SDL_GPUSampler* handle = SDL_CreateGPUSampler(dev, &sampler_info);
            if(!handle)
            {
                throw error("Failed to create GPU sampler");
            }
            return handle;
        }
    } // namespace

    void sampler::release::operator()(SDL_GPUSampler* handle) const
    {
        SDL_ReleaseGPUSampler(device, handle);
    }

    sampler::sampler(const device& dev, filter_t min_filter, filter_t mag_filter, sampler_address_mode_t address_mode)
        : handle_(create_sampler(dev.get(), static_cast<SDL_GPUFilter>(min_filter.value),
                                 static_cast<SDL_GPUFilter>(mag_filter.value),
                                 static_cast<SDL_GPUSamplerAddressMode>(address_mode.value)),
                  release{dev.get()})
    {
    }

    SDL_GPUSampler* sampler::get() const
    {
        return handle_.get();
    }
} // namespace sdl
