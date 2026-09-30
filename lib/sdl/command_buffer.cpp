#include "command_buffer.hpp"
#include "device.hpp"
#include "error.hpp"
#include "optional.hpp"
#include "texture.hpp"
#include "window.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace
    {
        SDL_GPUCommandBuffer* acquire_command_buffer(SDL_GPUDevice* dev)
        {
            SDL_GPUCommandBuffer* handle = SDL_AcquireGPUCommandBuffer(dev);
            if(!handle)
            {
                throw error("Failed to acquire command buffer");
            }
            return handle;
        }
    } // namespace

    void command_buffer::release::operator()(SDL_GPUCommandBuffer* handle) const
    {
        if(!SDL_SubmitGPUCommandBuffer(handle))
        {
            SDL_LogError(SDL_LOG_CATEGORY_GPU, "Failed to submit GPU command buffer: %s", SDL_GetError());
        }
    }

    command_buffer::command_buffer(const device& dev) : handle_(acquire_command_buffer(dev.get()))
    {
    }

    SDL_GPUCommandBuffer* command_buffer::get() const
    {
        return handle_.get();
    }

    optional<texture> command_buffer::acquire_swapchain(const window& win)
    {
        SDL_GPUTexture* swapchain = nullptr;
        if(!SDL_WaitAndAcquireGPUSwapchainTexture(handle_.get(), win.get(), &swapchain, nullptr, nullptr))
        {
            throw error("Failed to acquire swapchain texture");
        }

        // swapchain is null when no texture is available yet (vsync pacing),
        // or when the window is minimized/occluded — caller skips the frame
        if(swapchain)
        {
            return texture(swapchain);
        }
        return {};
    }

    optional<texture> command_buffer::acquire_swapchain(const window& win, unsigned& width, unsigned& height)
    {
        SDL_GPUTexture* swapchain = nullptr;
        Uint32 swapchain_texture_width = 0;
        Uint32 swapchain_texture_height = 0;
        if(!SDL_WaitAndAcquireGPUSwapchainTexture(handle_.get(), win.get(), &swapchain, &swapchain_texture_width,
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
