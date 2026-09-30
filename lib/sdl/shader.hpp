#pragma once
#include "bitflags.hpp"
#include "opaque_typedef.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct SDL_GPUDevice;
struct SDL_GPUShader;

namespace sdl
{
    // ========================================================================
    // GPU Shader Stage (opaque typedef)
    // ========================================================================

    opaque_typedef(uint32_t, shader_stage_t);

    namespace shader_stage
    {
        extern const shader_stage_t vertex;
        extern const shader_stage_t fragment;
    }

    // ========================================================================
    // GPU Shader Format (bitflags)
    // ========================================================================

    bitflags_typedef(uint32_t, shader_format_t);

    namespace shader_format
    {
        extern const shader_format_t invalid;
        extern const shader_format_t private_;
        extern const shader_format_t spirv;
        extern const shader_format_t dxbc;
        extern const shader_format_t dxil;
        extern const shader_format_t msl;
        extern const shader_format_t metallib;
    }

    class device;

    /**
     * RAII wrapper for SDL_GPUShader
     *
     * Manages shader creation and cleanup.
     * Non-copyable, moveable.
     */
    class shader
    {
        // Releases the handle on the device that created it.
        struct release
        {
            SDL_GPUDevice* device;
            void operator()(SDL_GPUShader* handle) const;
        };

        std::unique_ptr<SDL_GPUShader, release> handle_;

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

        // Non-copyable
        shader(const shader&) = delete;
        shader& operator=(const shader&) = delete;

        // Moveable
        shader(shader&& other) noexcept = default;
        shader& operator=(shader&& other) noexcept = default;

        /**
         * Get underlying SDL_GPUShader handle.
         *
         * @return Raw shader pointer (non-owning)
         */
        SDL_GPUShader* get() const;
    };

} // namespace sdl
