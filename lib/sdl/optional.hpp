#pragma once

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace sdl
{
    /**
     * A simple C++11-compatible optional<T> implementation.
     *
     * Provides a subset of the std::optional interface sufficient for
     * holding an optional value that may or may not be present.
     */
    template <typename T>
    class optional
    {
        typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
        bool engaged;

        T* ptr()
        {
            return reinterpret_cast<T*>(&storage);
        }

        const T* ptr() const
        {
            return reinterpret_cast<const T*>(&storage);
        }

        void destroy()
        {
            if(engaged)
            {
                ptr()->~T();
                engaged = false;
            }
        }

    public:
        // Default constructor - empty optional
        optional() : engaged(false)
        {
        }

        // Construct from value
        optional(const T& value) : engaged(true)
        {
            new(&storage) T(value);
        }

        optional(T&& value) : engaged(true)
        {
            new(&storage) T(std::move(value));
        }

        // Copy constructor
        optional(const optional& other) : engaged(false)
        {
            if(other.engaged)
            {
                new(&storage) T(*other.ptr());
                engaged = true;
            }
        }

        // Move constructor
        optional(optional&& other) noexcept : engaged(false)
        {
            if(other.engaged)
            {
                new(&storage) T(std::move(*other.ptr()));
                engaged = true;
                other.destroy();
            }
        }

        // Destructor
        ~optional()
        {
            destroy();
        }

        // Copy assignment
        optional& operator=(const optional& other)
        {
            if(this != &other)
            {
                destroy();
                if(other.engaged)
                {
                    new(&storage) T(*other.ptr());
                    engaged = true;
                }
            }
            return *this;
        }

        // Move assignment
        optional& operator=(optional&& other) noexcept
        {
            if(this != &other)
            {
                destroy();
                if(other.engaged)
                {
                    new(&storage) T(std::move(*other.ptr()));
                    engaged = true;
                    other.destroy();
                }
            }
            return *this;
        }

        // Assignment from value
        optional& operator=(const T& value)
        {
            destroy();
            new(&storage) T(value);
            engaged = true;
            return *this;
        }

        optional& operator=(T&& value)
        {
            destroy();
            new(&storage) T(std::move(value));
            engaged = true;
            return *this;
        }

        // Check if value is present
        bool has_value() const
        {
            return engaged;
        }

        explicit operator bool() const
        {
            return engaged;
        }

        // Access the value (throws if empty)
        T& value()
        {
            if(!engaged)
            {
                throw std::runtime_error("optional has no value");
            }
            return *ptr();
        }

        const T& value() const
        {
            if(!engaged)
            {
                throw std::runtime_error("optional has no value");
            }
            return *ptr();
        }

        // Unchecked access
        T& operator*()
        {
            return *ptr();
        }

        const T& operator*() const
        {
            return *ptr();
        }

        T* operator->()
        {
            return ptr();
        }

        const T* operator->() const
        {
            return ptr();
        }

        // Reset to empty state
        void reset()
        {
            destroy();
        }
    };

} // namespace sdl
