#pragma once

// one opaque typedef cannot be substituted for another one, even with the same underlying type
#define opaque_typedef(U, T) typedef opaque_type<struct U##_##T, U> T

template <typename TAG, typename T>
struct opaque_type
{
    T value;
    explicit opaque_type(T v) : value(v)
    {
    }
    friend bool operator==(opaque_type a, opaque_type b)
    {
        return a.value == b.value;
    }
    friend bool operator!=(opaque_type a, opaque_type b)
    {
        return a.value != b.value;
    }
    friend bool operator<(opaque_type a, opaque_type b)
    {
        return a.value < b.value;
    }
};

#include <cstddef>
#include <functional>

// hash function (for unordered_map)
namespace std
{
    template <typename N, typename T>
    struct hash<opaque_type<N, T>>
    {
        std::size_t operator()(const opaque_type<N, T>& x) const
        {
            return std::hash<T>()(x.value);
        }
    };
}
