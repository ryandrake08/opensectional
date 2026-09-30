#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

struct SDL_GPUDevice;
struct SDL_GPUTransferBuffer;

namespace sdl
{
    class device;

    /**
     * RAII wrapper for mapped transfer buffer.
     *
     * Used for uploading data to GPU buffers. Data can be appended
     * sequentially using append(). The buffer tracks the current
     * insertion point to prevent writing beyond capacity.
     *
     * Automatically unmaps on destruction.
     */
    class transfer_buffer
    {
        // Releases the handle on the device that created it.
        struct release
        {
            SDL_GPUDevice* device;
            void operator()(SDL_GPUTransferBuffer* handle) const;
        };

        SDL_GPUDevice* device_; // Non-owning (for map/unmap)
        std::unique_ptr<SDL_GPUTransferBuffer, release> handle_;
        uint32_t capacity_;
        uint32_t offset_ = 0;

    public:
        /**
         * Create and map transfer buffer for upload.
         *
         * @param dev GPU device
         * @param size Size in bytes
         * @throws std::runtime_error if creation or mapping fails
         */
        transfer_buffer(const device& dev, uint32_t size);

        /**
         * Unmap and destroy transfer buffer.
         */
        ~transfer_buffer();

        // Non-copyable
        transfer_buffer(const transfer_buffer&) = delete;
        transfer_buffer& operator=(const transfer_buffer&) = delete;

        // Moveable
        transfer_buffer(transfer_buffer&& other) noexcept = default;
        transfer_buffer& operator=(transfer_buffer&& other) noexcept = default;

        /**
         * Append data to the transfer buffer.
         *
         * Copies data into the buffer at the current insertion point
         * and advances the insertion point.
         *
         * @param data Pointer to data to copy
         * @param size Size of data in bytes
         * @return Offset where the data was written (for use in copy operations)
         * @throws std::runtime_error if data would exceed buffer capacity
         */
        uint32_t append(const void* data, uint32_t size);

        /**
         * Get transfer buffer handle for copy operations.
         *
         * @return Raw transfer buffer pointer
         */
        SDL_GPUTransferBuffer* get() const;

        /**
         * Get total buffer capacity in bytes.
         */
        uint32_t capacity() const;

        /**
         * Get current insertion point (bytes written so far).
         */
        uint32_t size() const;

        /**
         * Get remaining space in bytes.
         */
        uint32_t remaining() const;
    };

} // namespace sdl
