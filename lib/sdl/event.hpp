#pragma once
#include "types.hpp"
#include <cstdint>
#include <functional>
#include <memory>

namespace sdl
{

    // Event listener interface for handling input and window events
    struct event_listener
    {
        virtual ~event_listener() = default;

        // input events
        virtual void key_event(input_key_t key, input_action_t action, input_mod_t mod);
        virtual void button_event(input_button_t button, input_action_t action, input_mod_t mod);
        virtual void cursor_position_event(double xpos, double ypos);
        virtual void scroll_event(double xoffset, double yoffset);

        // resize events
        virtual void framebuffer_size_event(int width, int height);
    };

    // Event manager that polls SDL events and dispatches to registered listeners
    class event_manager
    {
        // pimpl
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        event_manager();
        ~event_manager();

        // Register/unregister event listeners
        void add_listener(const std::shared_ptr<event_listener>& listener);
        void remove_listener(const std::shared_ptr<event_listener>& listener);

        // Set a hook that receives every raw event before dispatch.
        // The void* points to an SDL_Event.
        void set_raw_event_hook(std::function<void(const void*)> hook);

        // Block until at least one event arrives, then drain all pending
        // events before returning. Returns true if quit was requested.
        bool dispatch_events();

        // Push a quit event to terminate the event loop
        static void push_quit_event();

        // Custom typed-event API. SDL allocates a numeric event-type
        // space (SDL_RegisterEvents); the wrapper exposes that
        // directly, with a single integer `code` payload, and routes
        // arriving events of a registered type to a per-type handler
        // set on the manager. App-level signaling from background
        // threads to the main loop — whether to wake the loop or to
        // deliver semantic notifications — goes through this API.

        // Allocate one fresh user-event type number. Safe to call
        // from any thread; each call returns a distinct value.
        static std::uint32_t register_event_type();

        // Push a custom event with the given type and integer code.
        // Thread-safe; wakes SDL_WaitEvent. The event is delivered
        // through the handler set on the destination event_manager
        // for this type, if any.
        static void push_event(std::uint32_t type, int code);

        // Register a handler for a previously-registered event type.
        // The handler is invoked from the main thread inside
        // dispatch_events when an event of that type is drained.
        // Only one handler per type; later calls replace earlier
        // ones. Call from the main thread before any matching event
        // is pushed.
        void set_event_handler(std::uint32_t type, std::function<void(int code)> handler);
    };

} // namespace sdl
