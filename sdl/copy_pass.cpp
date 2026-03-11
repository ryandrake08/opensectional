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

namespace sdl
{
    // Copy pass implementation
    struct copy_pass::impl
    {
        SDL_GPUCopyPass* handle;    // Owning (ended on destruction)
        transfer_buffer& transfer;  // Reference to shared transfer buffer

        impl(SDL_GPUCommandBuffer* cmd, transfer_buffer& tbuf)
            : handle(SDL_BeginGPUCopyPass(cmd))
            , transfer(tbuf)
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
    };

    copy_pass::copy_pass(command_buffer& cmd, transfer_buffer& transfer) : pimpl(new impl(cmd.get(), transfer))
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

    texture copy_pass::create_and_upload_texture(const device& dev, const surface& surf)
    {
        // Create texture from surface dimensions
        texture tex(dev, surf);

        // Append pixel data to shared transfer buffer
        uint32_t offset = pimpl->transfer.append(surf.pixels(), surf.size());

        // Upload to texture
        SDL_GPUTextureTransferInfo source = {};
        source.transfer_buffer = pimpl->transfer.get();
        source.offset = offset;

        SDL_GPUTextureRegion destination = {};
        destination.texture = tex.get();
        destination.w = surf.width();
        destination.h = surf.height();
        destination.d = 1;

        SDL_UploadToGPUTexture(pimpl->handle, &source, &destination, false);

        return tex;
    }

    // Template implementation for create_and_upload_buffer
    template<typename T>
    buffer copy_pass::create_and_upload_buffer(const device& dev, buffer_usage_t usage, const std::vector<T>& data)
    {
        if(data.empty())
        {
            throw error("Cannot create buffer from empty data");
        }

        buffer buf(dev, usage, static_cast<uint32_t>(data.size()), sizeof(T));

        uint32_t byte_size = static_cast<uint32_t>(data.size() * sizeof(T));
        uint32_t offset = pimpl->transfer.append(data.data(), byte_size);

        SDL_GPUTransferBufferLocation source = {};
        source.transfer_buffer = pimpl->transfer.get();
        source.offset = offset;

        SDL_GPUBufferRegion destination = {};
        destination.buffer = buf.get();
        destination.offset = 0;
        destination.size = byte_size;

        SDL_UploadToGPUBuffer(pimpl->handle, &source, &destination, false);

        return buf;
    }

    // Explicit template instantiations for the types we use
    template buffer copy_pass::create_and_upload_buffer<vertex_t2f_c4ub_v3f>(const device&, buffer_usage_t, const std::vector<vertex_t2f_c4ub_v3f>&);
    template buffer copy_pass::create_and_upload_buffer<int>(const device&, buffer_usage_t, const std::vector<int>&);
} // namespace sdl
