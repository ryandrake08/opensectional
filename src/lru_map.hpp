#pragma once

#include <cstddef>
#include <list>
#include <unordered_map>
#include <utility>

namespace osect
{
    // Map that keeps at most capacity entries, evicting the least recently
    // used. find() and put() both mark an entry most recently used. Not
    // thread-safe.
    template <typename key_t, typename value_t>
    class lru_map
    {
        using list_t = std::list<std::pair<key_t, value_t>>;

        list_t items_; // most recently used first
        std::unordered_map<key_t, typename list_t::iterator> index_;
        std::size_t capacity_;

    public:
        explicit lru_map(std::size_t capacity) : capacity_(capacity)
        {
        }

        // The value for key, marked most recently used, or null when absent.
        // The pointer stays valid until the entry is evicted or replaced.
        value_t* find(const key_t& key)
        {
            const auto it = index_.find(key);
            if(it == index_.end())
            {
                return nullptr;
            }
            items_.splice(items_.begin(), items_, it->second);
            return &it->second->second;
        }

        // Stores value for key as most recently used, replacing any existing
        // value, and evicts the least recently used entry beyond capacity.
        void put(const key_t& key, value_t value)
        {
            const auto it = index_.find(key);
            if(it != index_.end())
            {
                it->second->second = std::move(value);
                items_.splice(items_.begin(), items_, it->second);
                return;
            }
            items_.emplace_front(key, std::move(value));
            index_.emplace(key, items_.begin());
            if(index_.size() > capacity_)
            {
                index_.erase(items_.back().first);
                items_.pop_back();
            }
        }

        std::size_t size() const
        {
            return index_.size();
        }

        std::size_t capacity() const
        {
            return capacity_;
        }
    };
} // namespace osect
