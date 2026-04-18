#pragma once

#include <cstddef>
#include <functional>

#define bitflags_typedef(U, T) typedef bitflags<struct U ## _ ## T, U> T

template <typename TAG, typename T>
struct bitflags
{
    T value;
    explicit bitflags(T v) : value(v) {}
    bitflags() : value(0) {}

    friend bitflags operator|(bitflags a, bitflags b) { return bitflags(a.value | b.value); }
    friend bitflags operator&(bitflags a, bitflags b) { return bitflags(a.value & b.value); }
    friend bitflags operator~(bitflags a) { return bitflags(~a.value); }
    bitflags& operator|=(bitflags b) { value |= b.value; return *this; }
    bitflags& operator&=(bitflags b) { value &= b.value; return *this; }

    explicit operator bool() const { return value != 0; }
    friend bool operator==(bitflags a, bitflags b) { return a.value == b.value; }
    friend bool operator!=(bitflags a, bitflags b) { return a.value != b.value; }
};

namespace std
{
    template <typename N, typename T>
    struct hash<bitflags<N, T>>
    {
        std::size_t operator()(const bitflags<N, T>& x) const
        {
            return std::hash<T>()(x.value);
        }
    };
}
