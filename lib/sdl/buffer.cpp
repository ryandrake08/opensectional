#include "buffer.hpp"
#include "device.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace buffer_usage
    {
        const buffer_usage_t vertex(SDL_GPU_BUFFERUSAGE_VERTEX);
        const buffer_usage_t index(SDL_GPU_BUFFERUSAGE_INDEX);
        const buffer_usage_t indirect(SDL_GPU_BUFFERUSAGE_INDIRECT);
        const buffer_usage_t graphics_storage_read(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        const buffer_usage_t compute_storage_read(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        const buffer_usage_t compute_storage_write(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
    }

    namespace
    {
        SDL_GPUBuffer* create_buffer(SDL_GPUDevice* dev, buffer_usage_t usage, uint32_t byte_size)
        {
            SDL_GPUBufferCreateInfo info = {};
            info.usage = static_cast<SDL_GPUBufferUsageFlags>(usage.value);
            info.size = byte_size;

            SDL_GPUBuffer* handle = SDL_CreateGPUBuffer(dev, &info);
            if(!handle)
            {
                throw error("Failed to create GPU buffer");
            }

            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "GPU buffer created: %u bytes", byte_size);
            return handle;
        }
    } // namespace

    void buffer::release::operator()(SDL_GPUBuffer* handle) const
    {
        SDL_ReleaseGPUBuffer(device, handle);
    }

    buffer::buffer(const device& dev, buffer_usage_t usage, uint32_t num, uint32_t size)
        : handle_(create_buffer(dev.get(), usage, num * size), release{dev.get()}), count_(num), byte_size_(num * size)
    {
    }

    SDL_GPUBuffer* buffer::get() const
    {
        return handle_.get();
    }

    uint32_t buffer::count() const
    {
        return count_;
    }

    uint32_t buffer::byte_size() const
    {
        return byte_size_;
    }
} // namespace sdl
