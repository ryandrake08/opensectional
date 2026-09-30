#pragma once

#include "shader.hpp"
#include "texture.hpp"
#include <memory>
#include <string>

struct SDL_GPUDevice;
struct SDL_Window;

namespace sdl
{
    class window;

    /**
     * RAII wrapper for SDL_GPUDevice
     *
     * Manages device creation, window claiming, and cleanup.
     * Non-copyable, moveable.
     *
     * Constructor takes sdl::window reference to enforce that the window
     * was created before the GPU device (compile-time check).
     */
    class device
    {
        // Releases the window from the device, then destroys the device.
        struct release
        {
            SDL_Window* window;
            void operator()(SDL_GPUDevice* handle) const;
        };

        SDL_Window* window_; // Non-owning
        std::unique_ptr<SDL_GPUDevice, release> handle_;

    public:
        /**
         * Create GPU device and claim window for rendering.
         *
         * @param win SDL window wrapper (enforces creation order)
         * @param preferred_driver Force a specific backend (e.g. "vulkan",
         *                         "direct3d12"), or nullptr for auto-selection
         * @param vsync Enable vsync (default: false for lowest latency)
         * @param debug_mode Request backend debug/validation features
         *                   (default: false; on Vulkan, requires a Vulkan
         *                   loader and the Khronos validation layer to be
         *                   reachable at runtime)
         * @throws std::runtime_error if device creation or window claim fails
         */
        explicit device(const sdl::window& win, const char* preferred_driver, bool vsync = false,
                        bool debug_mode = false);

        // Non-copyable
        device(const device&) = delete;
        device& operator=(const device&) = delete;

        // Moveable
        device(device&& other) noexcept = default;
        device& operator=(device&& other) noexcept = default;

        /**
         * Get underlying SDL_GPUDevice handle.
         *
         * @return Raw device pointer (non-owning)
         */
        SDL_GPUDevice* get() const;

        /**
         * Get shader format for this device.
         *
         * @return Shader format (METALLIB on macOS, SPIRV on Linux, DXIL on Windows)
         */
        shader_format_t get_shader_format() const;

        /**
         * Get swapchain texture format for window.
         *
         * @return Texture format for swapchain
         */
        texture_format_t get_swapchain_format() const;

        /**
         * Get name of the backend driver being used.
         *
         * @return Backend name (e.g., "Metal", "Vulkan", "D3D12")
         */
        std::string get_backend_name() const;
    };

} // namespace sdl
