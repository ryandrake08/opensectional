#include "transfer_buffer.hpp"
#include "device.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    // Transfer buffer implementation
    struct transfer_buffer::impl
    {
        SDL_GPUDevice* device;         // Non-owning
        SDL_GPUTransferBuffer* handle; // Owning
        uint32_t buffer_capacity;
        uint32_t current_offset;

        static SDL_GPUTransferBuffer* create_transfer_buffer(SDL_GPUDevice* dev, uint32_t size)
        {
            // Create transfer buffer for upload
            SDL_GPUTransferBufferCreateInfo transfer_info = {};
            transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            transfer_info.size = size;

            return SDL_CreateGPUTransferBuffer(dev, &transfer_info);
        }

        impl(SDL_GPUDevice* dev, uint32_t size)
            : device(dev)
            , handle(create_transfer_buffer(dev, size))
            , buffer_capacity(size)
            , current_offset(0)
        {
            if(!handle)
            {
                throw error("Failed to create transfer buffer");
            }
        }

        ~impl() noexcept
        {
            if(current_offset != buffer_capacity)
            {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Transfer buffer not fully used: %u of %u bytes (%u unused)", current_offset, buffer_capacity, buffer_capacity - current_offset);
            }
            SDL_ReleaseGPUTransferBuffer(device, handle);
        }
    };

    transfer_buffer::transfer_buffer(const device& dev, uint32_t size) : pimpl(new impl(dev.get(), size))
    {
    }

    transfer_buffer::~transfer_buffer() = default;

    transfer_buffer::transfer_buffer(transfer_buffer&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    transfer_buffer& transfer_buffer::operator=(transfer_buffer&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    uint32_t transfer_buffer::append(const void* data, uint32_t size)
    {
        if(pimpl->current_offset + size > pimpl->buffer_capacity)
        {
            throw error("Transfer buffer overflow: cannot append " + std::to_string(size) + " bytes at offset " + std::to_string(pimpl->current_offset) + " (capacity: " + std::to_string(pimpl->buffer_capacity) + ")");
        }

        // Map the buffer
        void* mapped_data = SDL_MapGPUTransferBuffer(pimpl->device, pimpl->handle, false);
        if(!mapped_data)
        {
            throw error("Failed to map transfer buffer");
        }

        // Copy data to the current position
        uint8_t* dest = static_cast<uint8_t*>(mapped_data) + pimpl->current_offset;
        SDL_memcpy(dest, data, size);

        // Unmap the buffer
        SDL_UnmapGPUTransferBuffer(pimpl->device, pimpl->handle);

        // Save offset to return
        uint32_t offset = pimpl->current_offset;

        // Advance insertion point
        pimpl->current_offset += size;

        return offset;
    }

    SDL_GPUTransferBuffer* transfer_buffer::get() const
    {
        return pimpl->handle;
    }

    uint32_t transfer_buffer::capacity() const
    {
        return pimpl->buffer_capacity;
    }

    uint32_t transfer_buffer::size() const
    {
        return pimpl->current_offset;
    }

    uint32_t transfer_buffer::remaining() const
    {
        return pimpl->buffer_capacity - pimpl->current_offset;
    }
} // namespace sdl
