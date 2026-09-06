#include "copy_pass.hpp"
#include "buffer.hpp"
#include "command_buffer.hpp"
#include "device.hpp"
#include "error.hpp"
#include "surface.hpp"
#include "texture.hpp"
#include "transfer_buffer.hpp"
#include "types.hpp"
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <vector>

namespace sdl
{
    struct copy_pass::impl
    {
        SDL_GPUCopyPass* handle;
        std::vector<transfer_buffer> transfers;

        explicit impl(SDL_GPUCommandBuffer* cmd) : handle(SDL_BeginGPUCopyPass(cmd))
        {
            if(!handle)
            {
                throw error("Failed to begin copy pass");
            }
        }

        ~impl() noexcept
        {
            SDL_EndGPUCopyPass(handle);
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    copy_pass::copy_pass(command_buffer& cmd) : pimpl(new impl(cmd.get()))
    {
    }

    copy_pass::~copy_pass() = default;

    copy_pass::copy_pass(copy_pass&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    copy_pass& copy_pass::operator=(copy_pass&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    SDL_GPUCopyPass* copy_pass::get() const
    {
        return pimpl->handle;
    }

    void copy_pass::upload_buffer(transfer_buffer& tb, const buffer& dest, const void* data)
    {
        uint32_t byte_size = dest.byte_size();
        uint32_t offset = tb.append(data, byte_size);

        SDL_GPUTransferBufferLocation source = {};
        source.transfer_buffer = tb.get();
        source.offset = offset;

        SDL_GPUBufferRegion destination = {};
        destination.buffer = dest.get();
        destination.offset = 0;
        destination.size = byte_size;

        SDL_UploadToGPUBuffer(pimpl->handle, &source, &destination, false);
    }

    void copy_pass::upload_texture(transfer_buffer& tb, const texture& dest, const surface& surf)
    {
        upload_texture(tb, dest, surf.pixels(), static_cast<uint32_t>(surf.width()), static_cast<uint32_t>(surf.height()),
                       surf.size());
    }

    void copy_pass::upload_texture(transfer_buffer& tb, const texture& dest, const void* pixels, uint32_t width,
                                   uint32_t height, uint32_t byte_size)
    {
        if(!pixels)
        {
            throw error("Cannot upload texture from null pixels");
        }
        if(width == 0 || height == 0)
        {
            throw error("Cannot upload texture with zero dimensions");
        }

        uint32_t offset = tb.append(pixels, byte_size);

        SDL_GPUTextureTransferInfo source = {};
        source.transfer_buffer = tb.get();
        source.offset = offset;

        SDL_GPUTextureRegion destination = {};
        destination.texture = dest.get();
        destination.w = width;
        destination.h = height;
        destination.d = 1;

        SDL_UploadToGPUTexture(pimpl->handle, &source, &destination, false);
    }

    texture copy_pass::create_and_upload_texture(const device& dev, const surface& surf)
    {
        texture tex(dev, surf);
        pimpl->transfers.emplace_back(dev, surf.size());
        upload_texture(pimpl->transfers.back(), tex, surf);
        return tex;
    }

    buffer copy_pass::create_and_upload_buffer_raw(const device& dev, buffer_usage_t usage, const void* data,
                                                   uint32_t count, uint32_t element_size)
    {
        if(count == 0)
        {
            throw error("Cannot create buffer from empty data");
        }

        buffer buf(dev, usage, count, element_size);
        pimpl->transfers.emplace_back(dev, buf.byte_size());
        upload_buffer(pimpl->transfers.back(), buf, data);
        return buf;
    }

} // namespace sdl
