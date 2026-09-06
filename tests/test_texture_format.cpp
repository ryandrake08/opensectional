#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "sdl/copy_pass.hpp"
#include "sdl/texture.hpp"
#include "sdl/types.hpp"
#include <SDL3/SDL.h>
#include <type_traits>

static_assert(std::is_constructible<sdl::texture, const sdl::device&, unsigned, unsigned, sdl::texture_format_t>::value,
              "texture must support explicit dimensions and format");
static_assert(std::is_same<decltype(static_cast<void (sdl::copy_pass::*)(sdl::transfer_buffer&, const sdl::texture&,
                                                                          const void*, uint32_t, uint32_t, uint32_t)>(
                               &sdl::copy_pass::upload_texture)),
                           void (sdl::copy_pass::*)(sdl::transfer_buffer&, const sdl::texture&, const void*, uint32_t,
                                                    uint32_t, uint32_t)>::value,
              "copy_pass must support raw texture uploads");

TEST_CASE("texture format constants match SDL colour formats")
{
    CHECK(sdl::texture_format::r8_unorm.value == SDL_GPU_TEXTUREFORMAT_R8_UNORM);
    CHECK(sdl::texture_format::r8g8_unorm.value == SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);
    CHECK(sdl::texture_format::r8g8b8a8_unorm.value == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    CHECK(sdl::texture_format::r16_unorm.value == SDL_GPU_TEXTUREFORMAT_R16_UNORM);
    CHECK(sdl::texture_format::r16g16_unorm.value == SDL_GPU_TEXTUREFORMAT_R16G16_UNORM);
    CHECK(sdl::texture_format::r16g16b16a16_unorm.value == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM);
    CHECK(sdl::texture_format::r10g10b10a2_unorm.value == SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM);
    CHECK(sdl::texture_format::r8g8b8a8_unorm_srgb.value == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB);
    CHECK(sdl::texture_format::d16_unorm.value == SDL_GPU_TEXTUREFORMAT_D16_UNORM);
    CHECK(sdl::texture_format::d24_unorm.value == SDL_GPU_TEXTUREFORMAT_D24_UNORM);
    CHECK(sdl::texture_format::d32_float.value == SDL_GPU_TEXTUREFORMAT_D32_FLOAT);
    CHECK(sdl::texture_format::d24_unorm_s8_uint.value == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT);
    CHECK(sdl::texture_format::d32_float_s8_uint.value == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT);
}
