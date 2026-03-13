#pragma once
#include "types.hpp"
#include <SDL3/SDL_events.h>
#include <functional>
#include <memory>

namespace sdl
{

    // Event listener interface for handling input and window events
    struct event_listener
    {
        virtual ~event_listener() = default;

        // input events
        virtual void key_event(input_key_t key, input_action_t action, input_mod_t mod) = 0;
        virtual void button_event(input_button_t button, input_action_t action, input_mod_t mod) = 0;
        virtual void cursor_position_event(double xpos, double ypos) = 0;
        virtual void scroll_event(double xoffset, double yoffset) = 0;

        // resize events
        virtual void framebuffer_size_event(int width, int height) = 0;
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

        // Set a hook that receives every raw SDL_Event before dispatch
        void set_raw_event_hook(std::function<void(const SDL_Event&)> hook);

        // Block until an event arrives, then dispatch all pending events
        // Returns true if quit event was received
        bool wait_and_dispatch();

        // Push a user event to wake SDL_WaitEvent
        static void push_user_event();
    };

} // namespace sdl
