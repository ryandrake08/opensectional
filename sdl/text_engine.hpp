#pragma once
#include "types.hpp"
#include <glm/glm.hpp>
#include <memory>

namespace sdl
{
    // Forward declarations
    class device;

    /**
     * RAII wrapper for TTF_GPUTextEngine.
     *
     * Manages SDL3_ttf GPU text engine creation and cleanup.
     * Non-copyable, moveable.
     *
     * Constructor takes sdl::device reference to enforce that the GPU device
     * was created before the text engine (compile-time check).
     *
     * Usage:
     *   sdl::device dev(window);
     *   sdl::text_engine engine(dev);
     *   // TTF_DestroyGPUTextEngine called automatically on destruction
     */
    class text_engine
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Create GPU text engine.
         *
         * @param dev GPU device (enforces creation order)
         * @throws std::runtime_error if text engine creation fails
         */
        explicit text_engine(const device& dev);

        /**
         * Destroy text engine.
         */
        ~text_engine();

        // Non-copyable
        text_engine(const text_engine&) = delete;
        text_engine& operator=(const text_engine&) = delete;

        // Moveable
        text_engine(text_engine&& other) noexcept;
        text_engine& operator=(text_engine&& other) noexcept;

        /**
         * Get underlying TTF_TextEngine handle.
         *
         * @return Raw text engine pointer (non-owning)
         */
        TTF_TextEngine* get() const;

        // SDL3_ttf default atlas texture size
        static constexpr int ATLAS_SIZE = 1024;
    };

} // namespace sdl
