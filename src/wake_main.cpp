#include "wake_main.hpp"

#include <sdl/event.hpp>

namespace osect
{
    void wake_main_thread()
    {
        sdl::event_manager::push_user_event();
    }
}
