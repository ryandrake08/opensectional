#include "event.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace sdl
{
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

    struct event_manager::impl
    {
        std::vector<std::shared_ptr<event_listener>> listeners;
        std::function<void(const void*)> raw_event_hook;
        // Handlers for custom typed events allocated through
        // register_event_type(). The default case in dispatch()
        // checks this map after the built-in event types so a
        // SDL_EVENT_USER + N can route to per-app callbacks.
        std::unordered_map<std::uint32_t, std::function<void(int)>> typed_handlers;

        // Dispatch a single SDL event to listeners
        bool dispatch(const SDL_Event& event)
        {
            if(raw_event_hook)
            {
                raw_event_hook(&event);
            }

            switch(event.type)
            {
            case SDL_EVENT_QUIT:
                return true;

            case SDL_EVENT_WINDOW_RESIZED:
                for(const auto& listener : listeners)
                {
                    listener->framebuffer_size_event(event.window.data1, event.window.data2);
                }
                break;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            {
                input_action_t action =
                    (event.type == SDL_EVENT_KEY_DOWN) ? input_action::press : input_action::release;
                for(const auto& listener : listeners)
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
                for(const auto& listener : listeners)
                {
                    listener->button_event(input_button_t(event.button.button), action, input_mod_t(0));
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                for(const auto& listener : listeners)
                {
                    listener->cursor_position_event(event.motion.x, event.motion.y);
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                for(const auto& listener : listeners)
                {
                    listener->scroll_event(event.wheel.x, event.wheel.y);
                }
                break;

            default:
            {
                // Custom user events live in the SDL_EVENT_USER+N
                // range and are routed by exact type match.
                auto it = typed_handlers.find(event.type);
                if(it != typed_handlers.end())
                {
                    it->second(event.user.code);
                }
                break;
            }
            }

            return false;
        }
    };

    event_manager::event_manager() : pimpl(new impl())
    {
    }

    event_manager::~event_manager() = default;

    void event_manager::add_listener(const std::shared_ptr<event_listener>& listener)
    {
        this->pimpl->listeners.push_back(listener);
    }

    void event_manager::remove_listener(const std::shared_ptr<event_listener>& listener)
    {
        this->pimpl->listeners.erase(
            std::remove(this->pimpl->listeners.begin(), this->pimpl->listeners.end(), listener),
            this->pimpl->listeners.end());
    }

    void event_manager::set_raw_event_hook(std::function<void(const void*)> hook)
    {
        this->pimpl->raw_event_hook = std::move(hook);
    }

    bool event_manager::dispatch_events()
    {
        SDL_Event event;

        // Block until any event arrives.
        SDL_WaitEvent(&event);

        if(pimpl->dispatch(event))
        {
            return true;
        }

        while(SDL_PollEvent(&event))
        {
            if(pimpl->dispatch(event))
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
        pimpl->typed_handlers[type] = std::move(handler);
    }

} // namespace sdl
