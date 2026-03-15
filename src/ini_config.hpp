#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>

class ini_config
{
public:
    // observer base class
    struct observer
    {
        virtual ~observer();

        // called when a config varialbe changes
        virtual void observe_config_change(const std::string& section_dot_key, const std::string& from, const std::string& to) = 0;
    };

private:
    struct value_data
    {
        std::string value;
        std::unordered_set<observer*> observers;
    };

    // cache is a map of maps to hold section, keys, values, and callbacks
    typedef std::unordered_map<std::string, value_data> cache_type;

    // backing store filename
    std::string filename;

    // in-memory cache
    cache_type cache;

    // does cache need to sync to disk?
    bool needs_sync;

    // internal function to parse from an istream
    static cache_type parse(std::istream&& stream);

    // internal functions to find a section/key in the cache
    cache_type::const_iterator find(const std::string& section_dot_key) const;
    cache_type::iterator find(const std::string& section_dot_key);

public:
    // construction
    explicit ini_config(const std::string& filename);

    // get value for section.key
    template<typename T>
    T get(const std::string& section_dot_key) const;

    // set value for section.key
    template<typename T>
    void set(const std::string& section_dot_key, const T& value);

    // write cache to disk
    void write();

    // return true if section.key exists in config
    bool exists(const std::string& section_dot_key) const;

    // add/remove an observer for a section.key
    void observe(const std::string& section_dot_key, observer* ob);
    void unobserve(const std::string& section_dot_key, observer* ob);
};
