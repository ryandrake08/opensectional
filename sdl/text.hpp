#pragma once
#include "types.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

class color;

namespace sdl
{
    // Forward declarations
    class text_engine;
    class font;

    /**
     * Text bounding box in 2D space.
     *
     * Represents the rectangular bounds of rendered text geometry.
     * Coordinates match SDL3_ttf's convention: positive Y points UP.
     */
    struct text_bounds
    {
        float min_x, max_x, min_y, max_y;

        float width() const { return max_x - min_x; }
        float height() const { return max_y - min_y; }
    };

    /**
     * RAII wrapper for TTF_Text.
     *
     * Manages text object creation and cleanup.
     * Non-copyable, moveable.
     *
     * Constructor takes sdl::text_engine and sdl::font references to enforce
     * that both were created before the text (compile-time check).
     *
     * Usage:
     *   sdl::text_engine engine(dev);
     *   sdl::font my_font(engine, "font.ttf", 24);
     *   sdl::text my_text(engine, my_font, "Hello World!");
     *   // TTF_DestroyText called automatically on destruction
     */
    class text
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Create text object.
         *
         * @param engine Text engine (enforces creation order)
         * @param font Font to use (enforces creation order)
         * @param str Text string
         * @param length Text length (0 = null-terminated)
         * @throws std::runtime_error if text creation fails
         */
        text(const text_engine& engine, const font& font, const char* str, size_t length = 0);

        /**
         * Destroy text object.
         */
        ~text();

        // Non-copyable
        text(const text&) = delete;
        text& operator=(const text&) = delete;

        // Moveable
        text(text&& other) noexcept;
        text& operator=(text&& other) noexcept;

        /**
         * Get underlying TTF_Text handle.
         *
         * @return Raw text pointer (non-owning)
         */
        TTF_Text* get() const;

        /**
         * Get atlas texture for this text.
         *
         * Returns the raw SDL3_ttf atlas texture pointer.
         * The returned texture is valid only while this text object exists
         * and has valid draw data.
         *
         * @return Raw texture pointer, or nullptr if no draw data
         */
        SDL_GPUTexture* atlas_texture() const;

        /**
         * Update text string.
         *
         * @param str New text string
         * @param length Text length (0 = null-terminated)
         * @return true on success, false on failure
         */
        bool set_string(const char* str, size_t length = 0);

        /**
         * Get text bounding box with normalization and scaling.
         *
         * Normalizes font pixels by dividing by 800.0, then applies scale.
         *
         * @param scale Scale factor to apply after normalization
         * @return Bounds in normalized scaled coordinates, or all zeros if no geometry
         */
        text_bounds get_bounds(float scale) const;

        /**
         * Append text geometry to vertex and index buffers.
         *
         * Builds geometry for this text and appends it to the provided buffers.
         * Uses T2F_C4UB_V3F vertex format (texcoord, color, position).
         *
         * @param vertices Vertex buffer to append to
         * @param indices Index buffer to append to (indices adjusted for current vertex count)
         * @param position Position to place the text (in normalized coordinates)
         * @param scale Scale factor to apply to text
         * @param col Text color
         */
        void append_geometry(
            std::vector<vertex_t2f_c4ub_v3f>& vertices,
            std::vector<int>& indices,
            const glm::vec3& position,
            float scale,
            const color& col) const;
    };

} // namespace sdl
