#pragma once
#include <memory>

namespace sdl
{
    class device;
    class window;
    class command_buffer;
    class texture;
}

namespace imgui
{

    class context
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        context(sdl::device& dev, sdl::window& win);
        ~context();

        // Non-copyable, non-moveable
        context(const context&) = delete;
        context& operator=(const context&) = delete;

        // Process a raw SDL event (call from event hook)
        void process_event(const void* event);

        // Begin a new ImGui frame
        void new_frame();

        // Finalize the ImGui frame (must be called every frame after new_frame)
        void end_frame();

        // Render finalized ImGui draw data into a GPU pass on the swapchain texture
        void render(sdl::command_buffer& cmd, sdl::texture& swapchain);

        // Returns true if ImGui wants to capture mouse input
        bool wants_mouse() const;

        // Returns true if ImGui wants to capture keyboard input
        // (e.g. a text box has focus).
        bool wants_keyboard() const;

        // Consume one warmup-frame credit. Returns true if a forced
        // render is needed this frame. Caller is responsible for
        // ensuring the loop runs again next iteration while
        // warmup_pending() is still true (typically by pushing a
        // wake event into the SDL queue).
        bool warming_up();

        // Whether further warmup frames are still pending after the
        // most recent warming_up() call. Used by the caller to
        // decide whether to schedule another loop iteration.
        bool warmup_pending() const;
    };

} // namespace imgui
