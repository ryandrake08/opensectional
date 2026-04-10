#include "text.hpp"
#include "error.hpp"
#include "font.hpp"
#include "text_engine.hpp"
#include "types.hpp"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <algorithm>
#include <vector>

namespace sdl
{
    struct text::impl
    {
        TTF_Text* handle; // Owning

        impl(TTF_TextEngine* engine, TTF_Font* font, const char* str, size_t length) : handle(TTF_CreateText(engine, font, str, length))
        {
            if(!handle)
            {
                throw error("Failed to create text");
            }
        }

        ~impl() noexcept
        {
            TTF_DestroyText(handle);
        }
    };

    text::text(const text_engine& engine, const font& f, const char* str, size_t length) : pimpl(new impl(engine.get(), f.get(), str, length))
    {
    }

    text::~text() = default;

    text::text(text&& other) noexcept : pimpl(std::move(other.pimpl))
    {
    }

    text& text::operator=(text&& other) noexcept
    {
        if(this != &other)
        {
            pimpl = std::move(other.pimpl);
        }
        return *this;
    }

    TTF_Text* text::get() const
    {
        return pimpl->handle;
    }

    SDL_GPUTexture* text::atlas_texture() const
    {
        TTF_GPUAtlasDrawSequence* sequences = TTF_GetGPUTextDrawData(pimpl->handle);
        if(sequences)
        {
            return sequences->atlas_texture;
        }
        return nullptr;
    }

    bool text::set_string(const char* str, size_t length)
    {
        return TTF_SetTextString(pimpl->handle, str, length);
    }

    text_bounds text::get_bounds() const
    {
        text_bounds bounds = { 0.0F, 0.0F, 0.0F, 0.0F };

        // Get draw data from SDL3_ttf
        TTF_GPUAtlasDrawSequence* sequences = TTF_GetGPUTextDrawData(pimpl->handle);
        if(!sequences)
        {
            return bounds;
        }

        // Iterate through all sequences and vertices to find bounds
        bool first = true;
        for(TTF_GPUAtlasDrawSequence* seq = sequences; seq != nullptr; seq = seq->next)
        {
            for(int i = 0; i < seq->num_vertices; i++)
            {
                float x = seq->xy[i].x;
                float y = seq->xy[i].y;

                if(first)
                {
                    bounds.min_x = bounds.max_x = x;
                    bounds.min_y = bounds.max_y = y;
                    first = false;
                }
                else
                {
                    bounds.min_x = std::min(x, bounds.min_x);
                    bounds.max_x = std::max(x, bounds.max_x);
                    bounds.min_y = std::min(y, bounds.min_y);
                    bounds.max_y = std::max(y, bounds.max_y);
                }
            }
        }

        return bounds;
    }

    void text::append_geometry(
        std::vector<vertex_t2f_c4ub_v3f>& vertices,
        std::vector<int>& indices,
        const glm::vec3& position,
        unsigned char r, unsigned char g, unsigned char b, unsigned char a) const
    {
         // Get draw data from SDL3_ttf
       TTF_GPUAtlasDrawSequence* sequences = TTF_GetGPUTextDrawData(pimpl->handle);
        if(!sequences)
        {
            return;
        }

        // Iterate through all draw sequences
        for(TTF_GPUAtlasDrawSequence* seq = sequences; seq != nullptr; seq = seq->next)
        {
            if(seq->num_vertices == 0 || seq->num_indices == 0)
            {
                continue;
            }

            unsigned int vertex_offset = static_cast<unsigned int>(vertices.size());

            // Add vertices
            for(int i = 0; i < seq->num_vertices; i++)
            {
                vertex_t2f_c4ub_v3f vert;
                vert.s = seq->uv[i].x;
                vert.t = seq->uv[i].y;
                vert.r = r;
                vert.g = g;
                vert.b = b;
                vert.a = a;
                vert.x = position.x + seq->xy[i].x;
                vert.y = position.y + seq->xy[i].y;
                vert.z = position.z;
                vertices.push_back(vert);
            }

            // Add indices with offset
            for(int i = 0; i < seq->num_indices; i++)
            {
                indices.push_back(seq->indices[i] + vertex_offset);
            }
        }
    }
} // namespace sdl
