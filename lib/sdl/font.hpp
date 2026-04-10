#pragma once
#include "types.hpp"
#include <memory>

namespace sdl
{
    // Forward declaration
    class text_engine;

    /**
     * RAII wrapper for TTF_Font.
     *
     * Manages font loading and cleanup.
     * Non-copyable, moveable.
     *
     * Constructor takes sdl::text_engine reference to enforce that the text engine
     * was created before the font (compile-time check).
     *
     * Usage:
     *   sdl::text_engine engine(dev);
     *   sdl::font my_font(engine, "font.ttf", 24);
     *   // TTF_CloseFont called automatically on destruction
     */
    class font
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Load font from file.
         *
         * @param engine Text engine (enforces creation order)
         * @param path Path to font file
         * @param ptsize Point size to load
         * @throws std::runtime_error if font loading fails
         */
        font(const text_engine& engine, const char* path, int ptsize);

        /**
         * Load font from memory.
         *
         * @param engine Text engine (enforces creation order)
         * @param data Pointer to font data (must remain valid for font lifetime)
         * @param size Size of font data in bytes
         * @param ptsize Point size to load
         * @throws std::runtime_error if font loading fails
         */
        font(const text_engine& engine, const void* data, size_t size, int ptsize);

        /**
         * Close font.
         */
        ~font();

        // Non-copyable
        font(const font&) = delete;
        font& operator=(const font&) = delete;

        // Moveable
        font(font&& other) noexcept;
        font& operator=(font&& other) noexcept;

        /**
         * Get underlying TTF_Font handle.
         *
         * @return Raw font pointer (non-owning)
         */
        TTF_Font* get() const;

        /**
         * Set font outline size in pixels.
         *
         * @param outline Outline size (0 = no outline)
         */
        void set_outline(int outline);
    };
} // namespace sdl
