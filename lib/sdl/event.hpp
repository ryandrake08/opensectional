#pragma once
#include "bitflags.hpp"
#include "opaque_typedef.hpp"
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

union SDL_Event;

namespace sdl
{
    // ========================================================================
    // Input Types (opaque typedef)
    // ========================================================================

    opaque_typedef(int32_t, input_key_t);
    opaque_typedef(uint8_t, input_button_t);
    opaque_typedef(int, input_action_t);
    bitflags_typedef(uint16_t, input_mod_t);

    namespace input_action
    {
        extern const input_action_t release;
        extern const input_action_t press;
        extern const input_action_t repeat;
    }

    namespace input_mod
    {
        extern const input_mod_t shift;
        extern const input_mod_t control;
        extern const input_mod_t alt;
        extern const input_mod_t super;
    }

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
        std::vector<event_listener*> listeners_;
        std::function<void(const void*)> raw_event_hook_;
        // Handlers for custom typed events allocated through
        // register_event_type(). The default case in dispatch()
        // checks this map after the built-in event types so a
        // SDL_EVENT_USER + N can route to per-app callbacks.
        std::unordered_map<std::uint32_t, std::function<void(int)>> typed_handlers_;

        // Dispatch a single SDL event to listeners
        bool dispatch(const SDL_Event& event);

    public:
        event_manager() = default;

        // Non-copyable, non-movable: listeners are registered by reference
        event_manager(const event_manager&) = delete;
        event_manager& operator=(const event_manager&) = delete;
        event_manager(event_manager&&) = delete;
        event_manager& operator=(event_manager&&) = delete;

        // Register/unregister event listeners. The manager keeps a
        // reference, so a listener must stay alive while registered.
        void add_listener(event_listener& listener);
        void remove_listener(event_listener& listener);

        // Set a hook that receives every raw event before dispatch.
        // The void* points to an SDL_Event.
        void set_raw_event_hook(std::function<void(const void*)> hook);

        // Poll SDL events and dispatch to listeners without blocking.
        // Returns true if a quit event was received. Used by frame-rate
        // driven render loops that must run regardless of input activity.
        bool poll_and_dispatch();

        // Block until at least one event arrives, then drain all pending
        // events before returning. Returns true if quit was requested.
        bool wait_and_dispatch();

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
        // the dispatch methods when an event of that type is drained.
        // Only one handler per type; later calls replace earlier
        // ones. Call from the main thread before any matching event
        // is pushed.
        void set_event_handler(std::uint32_t type, std::function<void(int code)> handler);
    };

} // namespace sdl
