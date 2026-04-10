#pragma once

#include "types.hpp"
#include <memory>
#include <string>

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
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Create GPU device and claim window for rendering.
         *
         * @param win SDL window wrapper (enforces creation order)
         * @param vsync Enable vsync (default: false for lowest latency)
         * @param preferred_driver Force a specific backend (e.g. "vulkan",
         *                         "direct3d12"), or nullptr for auto-selection
         * @throws std::runtime_error if device creation or window claim fails
         */
        explicit device(const sdl::window& win, bool vsync = false, const char* preferred_driver = nullptr);

        /**
         * Destroy device and release window.
         */
        ~device();

        // Non-copyable
        device(const device&) = delete;
        device& operator=(const device&) = delete;

        // Moveable
        device(device&& other) noexcept;
        device& operator=(device&& other) noexcept;

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
