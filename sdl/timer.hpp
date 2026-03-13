#pragma once
#include <cstdint>

namespace sdl
{

    class timer
    {
        uint64_t start;

    public:
        timer();

        // Returns elapsed time since construction in milliseconds
        float elapsed_ms() const;
    };

} // namespace sdl
