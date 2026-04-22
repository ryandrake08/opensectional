#include "command_buffer.hpp"
#include "device.hpp"
#include "error.hpp"
#include "optional.hpp"
#include "texture.hpp"
#include "window.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    // Command buffer implementation
    struct command_buffer::impl
    {
        SDL_GPUDevice* device;        // Non-owning
        SDL_GPUCommandBuffer* handle; // Owning (submitted on destruction)

        impl(SDL_GPUDevice* dev) : device(dev), handle(SDL_AcquireGPUCommandBuffer(device))
        {
            if(!handle)
            {
                throw error("Failed to acquire command buffer");
            }
        }

        ~impl() noexcept
        {
            if(!SDL_SubmitGPUCommandBuffer(handle))
            {
                SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to submit GPU command buffer: %s", SDL_GetError());
            }
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    command_buffer::command_buffer(const device& dev) : pimpl(new impl(dev.get()))
    {
    }

    command_buffer::~command_buffer() = default;

    command_buffer::command_buffer(command_buffer&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    command_buffer& command_buffer::operator=(command_buffer&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    SDL_GPUCommandBuffer* command_buffer::get() const
    {
        return pimpl->handle;
    }

    optional<texture> command_buffer::acquire_swapchain(const window& win, unsigned& width, unsigned& height)
    {
        SDL_GPUTexture* swapchain = nullptr;
        Uint32 swapchain_texture_width = 0;
        Uint32 swapchain_texture_height = 0;
        if(!SDL_WaitAndAcquireGPUSwapchainTexture(pimpl->handle,
                                                  win.get(),
                                                  &swapchain,
                                                  &swapchain_texture_width,
                                                  &swapchain_texture_height))
        {
            throw error("Failed to acquire swapchain texture");
        }

        // swapchain is null when no texture is available yet (vsync pacing),
        // or when the window is minimized/occluded — caller skips the frame
        if(swapchain)
        {
            width = static_cast<unsigned>(swapchain_texture_width);
            height = static_cast<unsigned>(swapchain_texture_height);
            return texture(swapchain);
        }
        return {};
    }
} // namespace sdl
