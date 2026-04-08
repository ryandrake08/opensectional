#include "device.hpp"
#include "error.hpp"
#include "types.hpp"
#include "window.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    struct device::impl
    {
        SDL_Window* window;
        SDL_GPUDevice* handle;

        impl(SDL_Window* win, bool vsync) : window(win), handle(SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_DXIL, true, nullptr))
        {
            // Create GPU device with automatic backend selection
            if(!handle)
            {
                throw error("Failed to create GPU device");
            }

            // Claim window for rendering
            if(!SDL_ClaimWindowForGPUDevice(handle, window))
            {
                SDL_DestroyGPUDevice(handle);
                throw error("Failed to claim window for GPU device");
            }

            // Choose swapchain composition in order of preference:
            // 1. SDR_LINEAR - linear color space for correct blending (preferred)
            // 2. SDR - standard sRGB (fallback, always supported)
            SDL_GPUSwapchainComposition swapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
            const char* compositionName = "SDR";

            // Choose swapchain presentation mode in order of preference:
            // With vsync=false: IMMEDIATE > MAILBOX > VSYNC
            // With vsync=true:  MAILBOX > VSYNC
            SDL_GPUPresentMode swapchainPresentation = SDL_GPU_PRESENTMODE_VSYNC;
            const char* presentationName = "VSYNC";

            if(!vsync && SDL_WindowSupportsGPUPresentMode(handle, window, SDL_GPU_PRESENTMODE_IMMEDIATE))
            {
                swapchainPresentation = SDL_GPU_PRESENTMODE_IMMEDIATE;
                presentationName = "IMMEDIATE";
            }
            else if(SDL_WindowSupportsGPUPresentMode(handle, window, SDL_GPU_PRESENTMODE_MAILBOX))
            {
                swapchainPresentation = SDL_GPU_PRESENTMODE_MAILBOX;
                presentationName = "MAILBOX";
            }

            // Configure swapchain
            if(!SDL_SetGPUSwapchainParameters(handle, window, swapchainComposition, swapchainPresentation))
            {
                SDL_ReleaseWindowFromGPUDevice(handle, window);
                SDL_DestroyGPUDevice(handle);
                throw error("Failed to set swapchain parameters");
            }

            SDL_Log("  Swapchain composition: %s, present mode: %s", compositionName, presentationName);

            // Log GPU backend information
            const char* backend_name = SDL_GetGPUDeviceDriver(handle);
            SDL_Log("GPU device created: %s", backend_name ? backend_name : "Unknown");

            // Log supported shader formats
            SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(handle);
            std::string format_list;
            if(formats & SDL_GPU_SHADERFORMAT_METALLIB)
            {
                format_list += "MetalLib ";
            }
            if(formats & SDL_GPU_SHADERFORMAT_SPIRV)
            {
                format_list += "SPIR-V ";
            }
            if(formats & SDL_GPU_SHADERFORMAT_DXIL)
            {
                format_list += "DXIL ";
            }
            if(formats & SDL_GPU_SHADERFORMAT_MSL)
            {
                format_list += "MSL ";
            }
            if(!format_list.empty())
            {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "  Supported shader formats: %s", format_list.c_str());
            }
        }

        ~impl() noexcept
        {
            SDL_ReleaseWindowFromGPUDevice(handle, window);
            SDL_DestroyGPUDevice(handle);
        }
    };

    device::device(const sdl::window& win, bool vsync) : pimpl(new impl(win.get(), vsync))
    {
    }

    device::~device() = default;

    device::device(device&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    device& device::operator=(device&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    SDL_GPUDevice* device::get() const
    {
        return pimpl->handle;
    }

    shader_format_t device::get_shader_format() const
    {
        // Query supported shader formats
        SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(pimpl->handle);

        // Return the first supported format in order of preference
        // METALLIB > SPIRV > DXIL
        if(formats & SDL_GPU_SHADERFORMAT_METALLIB)
        {
            return shader_format::metallib;
        }
        if(formats & SDL_GPU_SHADERFORMAT_SPIRV)
        {
            return shader_format::spirv;
        }
        if(formats & SDL_GPU_SHADERFORMAT_DXIL)
        {
            return shader_format::dxil;
        }
        throw error("No supported shader formats found");
    }

    texture_format_t device::get_swapchain_format() const
    {
        return texture_format_t(SDL_GetGPUSwapchainTextureFormat(pimpl->handle, pimpl->window));
    }

    std::string device::get_backend_name() const
    {
        const char* driver = SDL_GetGPUDeviceDriver(pimpl->handle);
        return driver ? std::string(driver) : std::string("Unknown");
    }
} // namespace sdl
