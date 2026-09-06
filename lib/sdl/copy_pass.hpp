#pragma once

#include "buffer.hpp"
#include "types.hpp"
#include <memory>
#include <vector>

namespace sdl
{
    class command_buffer;
    class device;
    class surface;
    class texture;
    class transfer_buffer;

    /**
     * RAII wrapper for SDL_GPUCopyPass
     *
     * Begins copy pass on construction, ends on destruction.
     *
     * Two upload styles are supported:
     *
     *   1. upload_buffer / upload_texture take a caller-supplied transfer_buffer.
     *      The caller is responsible for sizing and lifetime, which allows N
     *      uploads to share a single SDL_GPUTransferBuffer allocation.
     *
     *   2. create_and_upload_* allocate a fresh transfer_buffer per call, owned
     *      by this copy_pass. Convenient for one-shot uploads; costlier per call.
     */
    class copy_pass
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        /**
         * Begin copy pass.
         *
         * @param cmd Command buffer
         * @throws std::runtime_error if copy pass creation fails
         */
        copy_pass(command_buffer& cmd);

        /**
         * End copy pass.
         */
        ~copy_pass();

        // Non-copyable
        copy_pass(const copy_pass&) = delete;
        copy_pass& operator=(const copy_pass&) = delete;

        // Moveable
        copy_pass(copy_pass&& other) noexcept;
        copy_pass& operator=(copy_pass&& other) noexcept;

        /**
         * Get underlying copy pass handle.
         *
         * @return Raw copy pass pointer (non-owning)
         */
        SDL_GPUCopyPass* get() const;

        /**
         * Append @p dest.byte_size() bytes from @p data to @p tb and record an
         * upload of the full destination buffer.
         *
         * The caller owns @p tb and must keep it alive at least until the
         * enclosing command buffer is submitted. Sharing one transfer_buffer
         * across multiple upload_* calls avoids per-upload allocations.
         *
         * @param tb   Caller-supplied transfer buffer (must have room for dest.byte_size())
         * @param dest Destination GPU buffer
         * @param data Pointer to source bytes (at least dest.byte_size() valid bytes)
         */
        void upload_buffer(transfer_buffer& tb, const buffer& dest, const void* data);

        /**
         * Vector overload of upload_buffer(). data.size() * sizeof(T) must
         * equal dest.byte_size().
         */
        template <typename T>
        void upload_buffer(transfer_buffer& tb, const buffer& dest, const std::vector<T>& data)
        {
            upload_buffer(tb, dest, data.data());
        }

        /**
         * Append @p surf pixels to @p tb and record an upload into @p dest.
         *
         * Same lifetime rules as upload_buffer().
         *
         * @param tb   Caller-supplied transfer buffer (must have room for surf.size())
         * @param dest Destination GPU texture (must match surf dimensions)
         * @param surf Source surface (RGBA8888)
         */
        void upload_texture(transfer_buffer& tb, const texture& dest, const surface& surf);

        /**
         * Append tightly packed pixels to @p tb and upload them to @p dest.
         *
         * @p byte_size must match the pixel format and dimensions of @p dest.
         */
        void upload_texture(transfer_buffer& tb, const texture& dest, const void* pixels, uint32_t width,
                            uint32_t height, uint32_t byte_size);

        /**
         * Create GPU buffer and upload data from vector.
         *
         * Allocates a fresh SDL_GPUTransferBuffer per call, kept alive by this
         * copy_pass. For hot paths uploading many small buffers in one frame,
         * prefer constructing the destination buffer at the call site and using
         * upload_buffer() with a shared transfer_buffer.
         *
         * @param dev GPU device
         * @param usage Buffer usage flags
         * @param data Vector of data to upload (must not be empty)
         * @return Created and populated buffer
         */
        template <typename T>
        buffer create_and_upload_buffer(const device& dev, buffer_usage_t usage, const std::vector<T>& data)
        {
            return create_and_upload_buffer_raw(dev, usage, data.data(), static_cast<uint32_t>(data.size()),
                                                static_cast<uint32_t>(sizeof(T)));
        }

        /**
         * Create GPU texture and upload surface data.
         *
         * Allocates a fresh SDL_GPUTransferBuffer per call, kept alive by this
         * copy_pass. For hot paths uploading many textures in one frame, prefer
         * constructing the destination texture at the call site and using
         * upload_texture() with a shared transfer_buffer.
         *
         * @param dev GPU device
         * @param surf Surface containing pixel data (RGBA8888 format)
         * @return Created and populated texture
         */
        texture create_and_upload_texture(const device& dev, const surface& surf);

    private:
        buffer create_and_upload_buffer_raw(const device& dev, buffer_usage_t usage, const void* data, uint32_t count,
                                            uint32_t element_size);
    };

} // namespace sdl
