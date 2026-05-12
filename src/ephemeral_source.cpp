#include "ephemeral_source.hpp"
#include <sdl/event.hpp>

namespace osect
{
    std::uint32_t ephemeral_refresh_event_type()
    {
        // Function-local static initialization is thread-safe in
        // C++11+; multiple background threads racing the first call
        // all observe the same registered type.
        static const std::uint32_t type = sdl::event_manager::register_event_type();
        return type;
    }

    void push_ephemeral_refresh(ephemeral_source source)
    {
        sdl::event_manager::push_event(ephemeral_refresh_event_type(), static_cast<int>(source));
    }
}
