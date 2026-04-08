#include "buffer.hpp"
#include "device.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    // Buffer implementation
    struct buffer::impl
    {
        SDL_GPUDevice* device; // Non-owning
        SDL_GPUBuffer* handle; // Owning
        Uint32 count;          // Store SDL type internally

        static SDL_GPUBuffer* create_buffer(SDL_GPUDevice* dev, SDL_GPUBufferUsageFlags usage, uint32_t size)
        {
            SDL_GPUBufferCreateInfo info = {};
            info.usage = usage;
            info.size = size;

            return SDL_CreateGPUBuffer(dev, &info);
        }

        impl(SDL_GPUDevice* dev, SDL_GPUBufferUsageFlags usage, uint32_t num, uint32_t size) : device(dev), handle(create_buffer(dev, usage, num * size)), count(num)
        {
            if(!handle)
            {
                throw error("Failed to create GPU buffer");
            }

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GPU buffer created: %u bytes",
                         num * size);
        }

        ~impl() noexcept
        {
            SDL_ReleaseGPUBuffer(device, handle);
        }
    };

    buffer::buffer(const device& dev, buffer_usage_t usage, uint32_t num, uint32_t size) : pimpl(new impl(dev.get(),
                                                                                                          static_cast<SDL_GPUBufferUsageFlags>(usage),
                                                                                                          static_cast<Uint32>(num),
                                                                                                          static_cast<Uint32>(size)))
    {
    }

    buffer::~buffer() = default;

    buffer::buffer(buffer&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    buffer& buffer::operator=(buffer&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    SDL_GPUBuffer* buffer::get() const
    {
        return pimpl->handle;
    }

    uint32_t buffer::count() const
    {
        return static_cast<uint32_t>(pimpl->count);
    }
} // namespace sdl
