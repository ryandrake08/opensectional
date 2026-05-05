#pragma once
#include "types.hpp"
#include <cstddef>
#include <memory>
#include <string>

namespace sdl
{
    class device;

    /**
     * RAII wrapper for SDL_GPUShader
     *
     * Manages shader creation and cleanup.
     * Non-copyable, moveable.
     */
    class shader
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Create shader from embedded C array.
         *
         * @param dev GPU device
         * @param code_array Embedded shader bytecode array
         * @param code_len Length of array
         * @param entrypoint Entry point function name
         * @param stage Shader stage
         * @param format Shader format (auto-detected if not specified)
         * @param num_samplers Number of samplers this shader uses (default: 0)
         * @param num_storage_buffers Number of storage buffers this shader uses (default: 0)
         */
        shader(const device& dev, const unsigned char* code_array, unsigned int code_len, const std::string& entrypoint,
               shader_stage_t stage, shader_format_t format = shader_format::invalid, uint32_t num_samplers = 0,
               uint32_t num_storage_buffers = 0);

        /**
         * Destroy shader.
         */
        ~shader();

        // Non-copyable
        shader(const shader&) = delete;
        shader& operator=(const shader&) = delete;

        // Moveable
        shader(shader&& other) noexcept;
        shader& operator=(shader&& other) noexcept;

        /**
         * Get underlying SDL_GPUShader handle.
         *
         * @return Raw shader pointer (non-owning)
         */
        SDL_GPUShader* get() const;
    };

} // namespace sdl
