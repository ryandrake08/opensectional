#pragma once
#include <cstddef>
#include <list>
#include <stdexcept>
#include <unordered_map>

template<typename value_t>
class lru_set
{
    // our map will point to list iterators, which we will use when looking up/using items
    typedef typename std::list<value_t>::iterator list_iterator_t;

    // list of items in lru order (back is least recently used)
    std::list<value_t> lru_list;

    // map to quickly find objects in the list
    std::unordered_map<value_t, list_iterator_t> iterator_map;

    // pre-defined maximum size at which we start expelling items
    std::size_t max_size;

public:
    // construct a lru set with given maximum size
    explicit lru_set(std::size_t size_limit) : max_size(size_limit)
    {
    }

    // put an item into the lru set
    void put(const value_t& value)
    {
        // always push to front of the list
        this->lru_list.push_front(value);

        // it may already be in. look it up
        auto it(this->iterator_map.find(value));
        if(it != this->iterator_map.end())
        {
            // erase any existing item in the map (and it's corresponding entry in the list)
            this->lru_list.erase(it->second);
            this->iterator_map.erase(it);
        }

        // insert into map
        this->iterator_map[value] = this->lru_list.begin();

        // check size
        if(this->iterator_map.size() > this->max_size)
        {
            // if we are over max_size, erase last item in list rom both list and map
            auto last = this->lru_list.end();
            last--;
            this->iterator_map.erase(*last);
            this->lru_list.pop_back();
        }
    }

    // get an item from the lru set (moving it to the front of the list)
    const value_t& get(const value_t& value)
    {
        // find in map
        auto it(this->iterator_map.find(value));
        if(it == this->iterator_map.end())
        {
            throw std::range_error("value not in lru_set");
        }

        // move to front of the list and return
        this->lru_list.splice(this->lru_list.begin(), this->lru_list, it->second);
        return *it->second;
    }

    bool exists(const value_t& value) const
    {
        return this->iterator_map.find(value) != this->iterator_map.end();
    }

    std::size_t size() const
    {
        return this->iterator_map.size();
    }
};
