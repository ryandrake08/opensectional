#pragma once
#include "types.hpp"
#include <memory>

namespace sdl
{
    // Forward declaration
    class instance;

    /**
     * RAII wrapper for SDL_Window.
     *
     * Manages SDL_CreateWindow/SDL_DestroyWindow lifecycle.
     * Non-copyable, moveable.
     *
     * Constructor takes sdl::instance reference to enforce that SDL
     * was initialized before window creation (compile-time check).
     *
     * Usage:
     *   sdl::instance sdl_ctx(SDL_INIT_VIDEO);
     *   sdl::window window(sdl_ctx, "My Window", 800, 600);
     *   // Window is now created
     *   // SDL_DestroyWindow called automatically on destruction
     */
    class window
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Create SDL window.
         *
         * @param sdl SDL instance (enforces initialization order)
         * @param title Window title
         * @param width Window width in pixels
         * @param height Window height in pixels
         * @param flags SDL window flags (e.g., window_flags::resizable)
         * @throws std::runtime_error if window creation fails
         */
        window(const instance& inst, const char* title, int width, int height, window_flags_t flags);

        /**
         * Destroy window.
         */
        ~window();

        // Non-copyable
        window(const window&) = delete;
        window& operator=(const window&) = delete;

        // Moveable
        window(window&& other) noexcept;
        window& operator=(window&& other) noexcept;

        /**
         * Get underlying SDL_Window handle.
         *
         * @return Raw window pointer (non-owning)
         */
        SDL_Window* get() const;
    };

} // namespace sdl
