#include "transfer_buffer.hpp"
#include "device.hpp"
#include "error.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace
    {
        SDL_GPUTransferBuffer* create_transfer_buffer(SDL_GPUDevice* dev, uint32_t size)
        {
            // Create transfer buffer for upload
            SDL_GPUTransferBufferCreateInfo transfer_info = {};
            transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            transfer_info.size = size;

            SDL_GPUTransferBuffer* handle = SDL_CreateGPUTransferBuffer(dev, &transfer_info);
            if(!handle)
            {
                throw error("Failed to create transfer buffer");
            }
            return handle;
        }
    } // namespace

    void transfer_buffer::release::operator()(SDL_GPUTransferBuffer* handle) const
    {
        SDL_ReleaseGPUTransferBuffer(device, handle);
    }

    transfer_buffer::transfer_buffer(const device& dev, uint32_t size)
        : device_(dev.get()), handle_(create_transfer_buffer(dev.get(), size), release{dev.get()}), capacity_(size)
    {
    }

    transfer_buffer::~transfer_buffer()
    {
        if(handle_ && offset_ != capacity_)
        {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Transfer buffer not fully used: %u of %u bytes (%u unused)",
                         offset_, capacity_, capacity_ - offset_);
        }
    }

    uint32_t transfer_buffer::append(const void* data, uint32_t size)
    {
        if(offset_ + size > capacity_)
        {
            throw error("Transfer buffer overflow: cannot append " + std::to_string(size) + " bytes at offset " +
                        std::to_string(offset_) + " (capacity: " + std::to_string(capacity_) + ")");
        }

        // Map the buffer
        void* mapped_data = SDL_MapGPUTransferBuffer(device_, handle_.get(), false);
        if(!mapped_data)
        {
            throw error("Failed to map transfer buffer");
        }

        // Copy data to the current position
        uint8_t* dest = static_cast<uint8_t*>(mapped_data) + offset_;
        SDL_memcpy(dest, data, size);

        // Unmap the buffer
        SDL_UnmapGPUTransferBuffer(device_, handle_.get());

        // Save offset to return
        uint32_t offset = offset_;

        // Advance insertion point
        offset_ += size;

        return offset;
    }

    SDL_GPUTransferBuffer* transfer_buffer::get() const
    {
        return handle_.get();
    }

    uint32_t transfer_buffer::capacity() const
    {
        return capacity_;
    }

    uint32_t transfer_buffer::size() const
    {
        return offset_;
    }

    uint32_t transfer_buffer::remaining() const
    {
        return capacity_ - offset_;
    }
} // namespace sdl
