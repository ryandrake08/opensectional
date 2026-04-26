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

        static SDL_GPUDevice* create_device(SDL_Window* win, bool vsync, const char* preferred_driver)
        {
            if(preferred_driver)
            {
                SDL_SetHint(SDL_HINT_GPU_DRIVER, preferred_driver);
            }
            SDL_GPUShaderFormat formats_mask = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef __APPLE__
            formats_mask |= SDL_GPU_SHADERFORMAT_MSL;
#endif
#ifdef OSECT_HAVE_METALLIB
            formats_mask |= SDL_GPU_SHADERFORMAT_METALLIB;
#endif
#ifdef OSECT_HAVE_DXIL
            formats_mask |= SDL_GPU_SHADERFORMAT_DXIL;
#endif
            SDL_GPUDevice* dev = SDL_CreateGPUDevice(formats_mask, false, nullptr);
            if(!dev)
            {
                throw error("Failed to create GPU device");
            }

            if(!SDL_ClaimWindowForGPUDevice(dev, win))
            {
                SDL_DestroyGPUDevice(dev);
                throw error("Failed to claim window for GPU device");
            }

            // Choose swapchain composition in order of preference:
            // 1. SDR_LINEAR - linear color space for correct blending (preferred)
            // 2. SDR - standard sRGB (fallback, always supported)
            SDL_GPUSwapchainComposition swapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
            const char* compositionName = "SDR";

            // Choose swapchain presentation mode in order of preference:
            // With vsync=false: IMMEDIATE > MAILBOX > VSYNC
            // With vsync=true:  VSYNC
            SDL_GPUPresentMode swapchainPresentation = SDL_GPU_PRESENTMODE_VSYNC;
            const char* presentationName = "VSYNC";

            if(!vsync)
            {
                if(SDL_WindowSupportsGPUPresentMode(dev, win, SDL_GPU_PRESENTMODE_IMMEDIATE))
                {
                    swapchainPresentation = SDL_GPU_PRESENTMODE_IMMEDIATE;
                    presentationName = "IMMEDIATE";
                }
                else if(SDL_WindowSupportsGPUPresentMode(dev, win, SDL_GPU_PRESENTMODE_MAILBOX))
                {
                    swapchainPresentation = SDL_GPU_PRESENTMODE_MAILBOX;
                    presentationName = "MAILBOX";
                }
            }

            if(!SDL_SetGPUSwapchainParameters(dev, win, swapchainComposition, swapchainPresentation))
            {
                SDL_ReleaseWindowFromGPUDevice(dev, win);
                SDL_DestroyGPUDevice(dev);
                throw error("Failed to set swapchain parameters");
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Swapchain composition: %s, present mode: %s", compositionName, presentationName);

            const char* backend_name = SDL_GetGPUDeviceDriver(dev);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GPU device created: %s", backend_name ? backend_name : "Unknown");

            SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev);
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

            return dev;
        }

        impl(SDL_Window* win, bool vsync, const char* preferred_driver) : window(win), handle(create_device(win, vsync, preferred_driver)) {}

        ~impl() noexcept
        {
            SDL_ReleaseWindowFromGPUDevice(handle, window);
            SDL_DestroyGPUDevice(handle);
        }

        impl(const impl&) = delete;
        impl& operator=(const impl&) = delete;
        impl(impl&&) = default;
        impl& operator=(impl&&) = default;
    };

    device::device(const sdl::window& win, bool vsync, const char* preferred_driver) : pimpl(new impl(win.get(), vsync, preferred_driver))
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

        // Return the first supported format in order of preference.
        // METALLIB > MSL > SPIRV > DXIL. METALLIB is gated by whether the
        // build embedded precompiled bytecode (full Xcode), MSL by whether
        // the platform is macOS (always built when so).
#ifdef OSECT_HAVE_METALLIB
        if(formats & SDL_GPU_SHADERFORMAT_METALLIB)
        {
            return shader_format::metallib;
        }
#endif
#ifdef __APPLE__
        if(formats & SDL_GPU_SHADERFORMAT_MSL)
        {
            return shader_format::msl;
        }
#endif
        if(formats & SDL_GPU_SHADERFORMAT_SPIRV)
        {
            return shader_format::spirv;
        }
#ifdef OSECT_HAVE_DXIL
        if(formats & SDL_GPU_SHADERFORMAT_DXIL)
        {
            return shader_format::dxil;
        }
#endif
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
