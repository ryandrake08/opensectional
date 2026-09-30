#include "device.hpp"
#include "error.hpp"
#include "window.hpp"
#include <SDL3/SDL.h>

namespace sdl
{
    namespace
    {
        SDL_GPUDevice* create_device(SDL_Window* win, const char* preferred_driver, bool vsync, bool debug_mode)
        {
            SDL_GPUShaderFormat formats_mask = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef __APPLE__
            formats_mask |= SDL_GPU_SHADERFORMAT_MSL;
#endif
#ifdef SDL_WRAPPER_HAVE_METALLIB
            formats_mask |= SDL_GPU_SHADERFORMAT_METALLIB;
#endif
#ifdef SDL_WRAPPER_HAVE_DXIL
            formats_mask |= SDL_GPU_SHADERFORMAT_DXIL;
#endif
            SDL_GPUDevice* dev = SDL_CreateGPUDevice(formats_mask, debug_mode, preferred_driver);
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

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "  Swapchain composition: %s, present mode: %s", compositionName,
                        presentationName);

            const char* backend_name = SDL_GetGPUDeviceDriver(dev);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GPU device created: %s",
                        backend_name ? backend_name : "Unknown");

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
    } // namespace

    void device::release::operator()(SDL_GPUDevice* handle) const
    {
        SDL_ReleaseWindowFromGPUDevice(handle, window);
        SDL_DestroyGPUDevice(handle);
    }

    device::device(const sdl::window& win, const char* preferred_driver, bool vsync, bool debug_mode)
        : window_(win.get()), handle_(create_device(win.get(), preferred_driver, vsync, debug_mode), release{win.get()})
    {
    }

    SDL_GPUDevice* device::get() const
    {
        return handle_.get();
    }

    shader_format_t device::get_shader_format() const
    {
        // Query supported shader formats
        SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(handle_.get());

        // Return the first supported format in order of preference.
        // METALLIB > MSL > SPIRV > DXIL. METALLIB is gated by whether the
        // build embedded precompiled bytecode (full Xcode), MSL by whether
        // the platform is macOS (always built when so).
#ifdef SDL_WRAPPER_HAVE_METALLIB
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
#ifdef SDL_WRAPPER_HAVE_DXIL
        if(formats & SDL_GPU_SHADERFORMAT_DXIL)
        {
            return shader_format::dxil;
        }
#endif
        throw error("No supported shader formats found");
    }

    texture_format_t device::get_swapchain_format() const
    {
        return texture_format_t(SDL_GetGPUSwapchainTextureFormat(handle_.get(), window_));
    }

    std::string device::get_backend_name() const
    {
        const char* driver = SDL_GetGPUDeviceDriver(handle_.get());
        return driver ? std::string(driver) : std::string("Unknown");
    }
} // namespace sdl
