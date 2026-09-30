#include "event.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <vector>

namespace sdl
{
    namespace input_action
    {
        const input_action_t release(0); // SDL uses 0 for release
        const input_action_t press(1);   // SDL uses 1 for press
        const input_action_t repeat(2);  // Custom value for key repeat
    }

    namespace input_mod
    {
        const input_mod_t shift(SDL_KMOD_SHIFT);
        const input_mod_t control(SDL_KMOD_CTRL);
        const input_mod_t alt(SDL_KMOD_ALT);
        const input_mod_t super(SDL_KMOD_GUI);
    }

    void event_listener::key_event(input_key_t /*key*/, input_action_t /*action*/, input_mod_t /*mods*/)
    {
    }
    void event_listener::button_event(input_button_t /*button*/, input_action_t /*action*/, input_mod_t /*mods*/)
    {
    }
    void event_listener::cursor_position_event(double /*x*/, double /*y*/)
    {
    }
    void event_listener::scroll_event(double /*x*/, double /*y*/)
    {
    }
    void event_listener::framebuffer_size_event(int /*width*/, int /*height*/)
    {
    }

    bool event_manager::dispatch(const SDL_Event& event)
    {
        if(raw_event_hook_)
        {
            raw_event_hook_(&event);
        }

        switch(event.type)
        {
        case SDL_EVENT_QUIT:
            return true;

        case SDL_EVENT_WINDOW_RESIZED:
            for(const auto& listener : listeners_)
            {
                listener->framebuffer_size_event(event.window.data1, event.window.data2);
            }
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            input_action_t action = (event.type == SDL_EVENT_KEY_DOWN) ? input_action::press : input_action::release;
            for(const auto& listener : listeners_)
            {
                listener->key_event(input_key_t(event.key.key), action, input_mod_t(event.key.mod));
            }
            break;
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            input_action_t action =
                (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? input_action::press : input_action::release;
            for(const auto& listener : listeners_)
            {
                listener->button_event(input_button_t(event.button.button), action, input_mod_t(0));
            }
            break;
        }

        case SDL_EVENT_MOUSE_MOTION:
            for(const auto& listener : listeners_)
            {
                listener->cursor_position_event(event.motion.x, event.motion.y);
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            for(const auto& listener : listeners_)
            {
                listener->scroll_event(event.wheel.x, event.wheel.y);
            }
            break;

        default:
        {
            // Custom user events live in the SDL_EVENT_USER+N
            // range and are routed by exact type match.
            auto it = typed_handlers_.find(event.type);
            if(it != typed_handlers_.end())
            {
                it->second(event.user.code);
            }
            break;
        }
        }

        return false;
    }

    void event_manager::add_listener(event_listener& listener)
    {
        listeners_.push_back(&listener);
    }

    void event_manager::remove_listener(event_listener& listener)
    {
        listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), &listener), listeners_.end());
    }

    void event_manager::set_raw_event_hook(std::function<void(const void*)> hook)
    {
        raw_event_hook_ = std::move(hook);
    }

    bool event_manager::poll_and_dispatch()
    {
        SDL_Event event;
        while(SDL_PollEvent(&event))
        {
            if(dispatch(event))
            {
                return true;
            }
        }
        return false;
    }

    bool event_manager::wait_and_dispatch()
    {
        SDL_Event event;

        // Block until any event arrives.
        SDL_WaitEvent(&event);

        if(dispatch(event))
        {
            return true;
        }

        while(SDL_PollEvent(&event))
        {
            if(dispatch(event))
            {
                return true;
            }
        }

        return false;
    }

    void event_manager::push_quit_event()
    {
        SDL_Event ev = {};
        ev.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&ev);
    }

    std::uint32_t event_manager::register_event_type()
    {
        return SDL_RegisterEvents(1);
    }

    void event_manager::push_event(std::uint32_t type, int code)
    {
        SDL_Event ev = {};
        ev.user.type = type;
        ev.user.code = code;
        SDL_PushEvent(&ev);
    }

    void event_manager::set_event_handler(std::uint32_t type, std::function<void(int)> handler)
    {
        typed_handlers_[type] = std::move(handler);
    }

} // namespace sdl
