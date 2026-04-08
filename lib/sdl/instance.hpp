#pragma once

#include <memory>

namespace sdl
{
    /**
     * RAII wrapper for SDL initialization and cleanup.
     *
     * Manages SDL_Init/SDL_Quit lifecycle.
     * Non-copyable, moveable.
     *
     * Usage:
     *   sdl::instance sdl_ctx;
     *   // SDL is now initialized
     *   // ... create windows, etc.
     *   // SDL_Quit called automatically on destruction
     */
    class instance
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Initialize SDL subsystems.
         *
         * @param verbose When true, enables debug-level logging for
         *                resource lifecycle and GPU operations.
         * @throws std::runtime_error if SDL initialization fails
         */
        explicit instance(bool verbose = false);

        /**
         * Quit SDL subsystems.
         */
        ~instance();

        // Non-copyable
        instance(const instance&) = delete;
        instance& operator=(const instance&) = delete;

        // Moveable
        instance(instance&& other) noexcept;
        instance& operator=(instance&& other) noexcept;
    };

} // namespace sdl
