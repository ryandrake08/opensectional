#pragma once

namespace sdl
{
    /**
     * RAII wrapper for SDL initialization and cleanup.
     *
     * Manages SDL_Init/SDL_Quit lifecycle.
     * Non-copyable, non-movable.
     *
     * Usage:
     *   sdl::instance sdl_ctx;
     *   // SDL is now initialized
     *   // ... create windows, etc.
     *   // SDL_Quit called automatically on destruction
     */
    class instance
    {
    public:
        /**
         * Initialize SDL subsystems.
         *
         * @param verbosity Log verbosity level:
         *                  0 = errors only (default)
         *                  1 = warnings
         *                  2 = info
         *                  3 = debug (resource lifecycle, GPU ops)
         * @throws std::runtime_error if SDL initialization fails
         */
        explicit instance(int verbosity = 0);

        /**
         * Quit SDL subsystems.
         */
        ~instance();

        // Non-copyable, non-movable: SDL is initialized once per instance
        instance(const instance&) = delete;
        instance& operator=(const instance&) = delete;
        instance(instance&&) = delete;
        instance& operator=(instance&&) = delete;
    };

} // namespace sdl
