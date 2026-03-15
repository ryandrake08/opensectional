#include "ini_config.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

ini_config::observer::~observer() = default;

static std::string trim(const std::string& s, const std::string& whitespace = " \t", const std::string& comment = ";")
{
    auto s_without_comments(s);

    // chop off any comment
    const auto comm(s.find_first_of(comment));
    if(comm != std::string::npos)
    {
        s_without_comments = s.substr(0, s.find(comment, 0));
    }

    std::string s_trimmed;

    // find first whitespace
    const auto left(s_without_comments.find_first_not_of(whitespace));
    if(left != std::string::npos)
    {
        // find last whitespace
        const auto right(s_without_comments.find_last_not_of(whitespace));
        s_trimmed = s_without_comments.substr(left, right - left + 1);
    }

    return s_trimmed;
}

static std::vector<std::string> split(const std::string& string, const char delimeter)
{
    std::vector<std::string> strings;
    std::istringstream f(string);
    std::string s;

    while(std::getline(f, s, delimeter))
    {
        strings.push_back(s);
    }

    return strings;
}

ini_config::cache_type ini_config::parse(std::istream&& stream)
{
    // rewind stream
    stream.clear();
    stream.seekg(0);

    ini_config::cache_type model;
    std::string line;
    std::string last_section;

    while(std::getline(stream, line))
    {
        auto start(line.find('['));
        if(start != std::string::npos)
        {
            auto end(line.find(']'));
            last_section = line.substr(start + 1, end - 1);
        }
        else if(!last_section.empty())
        {
            auto equals(line.find('='));
            if(equals != std::string::npos)
            {
                auto parts(split(line, '='));
                if(parts.size() > 2)
                {
                    throw std::runtime_error("invalid ini file");
                }

                auto key(trim(parts[0]));
                auto value(trim(parts[1]));
                std::string full_key = last_section;
                full_key += '.';
                full_key += key;
                model[full_key].value = value;
            }
        }
    }

    return model;
}

ini_config::cache_type::const_iterator ini_config::find(const std::string& section_dot_key) const
{
    // find key
    auto key_it(this->cache.find(section_dot_key));
    if(key_it == this->cache.end())
    {
        throw std::runtime_error("ini_config: configuration file: " + filename + " contains no key: " + section_dot_key);
    }

    return key_it;
}

ini_config::cache_type::iterator ini_config::find(const std::string& section_dot_key)
{
    // find key
    auto key_it(this->cache.find(section_dot_key));
    if(key_it == this->cache.end())
    {
        throw std::runtime_error("ini_config: configuration file: " + filename + " contains no key: " + section_dot_key);
    }

    return key_it;
}

// construction
ini_config::ini_config(const std::string& filename) : filename(filename), cache(ini_config::parse(std::ifstream(filename))), needs_sync(false)
{
}

// get value for section.key
template<>
std::string ini_config::get(const std::string& section_dot_key) const
{
    // return the value
    return this->find(section_dot_key)->second.value;
}

template<>
int ini_config::get(const std::string& section_dot_key) const
{
    auto value(this->get<std::string>(section_dot_key));
    return std::stoi(value);
}

template<>
double ini_config::get(const std::string& section_dot_key) const
{
    auto value(this->get<std::string>(section_dot_key));
    return std::stod(value);
}

template<>
float ini_config::get(const std::string& section_dot_key) const
{
    auto value(this->get<std::string>(section_dot_key));
    return std::stof(value);
}

template<>
long ini_config::get(const std::string& section_dot_key) const
{
    auto value(this->get<std::string>(section_dot_key));
    return std::stol(value);
}

template<>
long double ini_config::get(const std::string& section_dot_key) const
{
    auto value(this->get<std::string>(section_dot_key));
    return std::stold(value);
}

template<>
long long ini_config::get(const std::string& section_dot_key) const
{
    auto value(this->get<std::string>(section_dot_key));
    return std::stoll(value);
}

template<>
unsigned long ini_config::get(const std::string& section_dot_key) const
{
    auto value(this->get<std::string>(section_dot_key));
    return std::stoul(value);
}

template<>
unsigned long long ini_config::get(const std::string& section_dot_key) const
{
    auto value(this->get<std::string>(section_dot_key));
    return std::stoull(value);
}

template<>
void ini_config::set(const std::string& section_dot_key, const std::string& value)
{
    // find key
    auto key_it(this->find(section_dot_key));

    // store old value
    auto old(key_it->second.value);

    // set the value
    key_it->second.value = value;

    // mark as needing to be synced to disk
    needs_sync = true;

    // call any associated observers
    for(const auto& ob : key_it->second.observers)
    {
        if(ob != nullptr)
        {
            ob->observe_config_change(section_dot_key, old, value);
        }
    }
}

template<>
void ini_config::set(const std::string& section_dot_key, const int& value)
{
    auto str(std::to_string(value));
    this->set<std::string>(section_dot_key, str);
}

template<>
void ini_config::set(const std::string& section_dot_key, const unsigned& value)
{
    auto str(std::to_string(value));
    this->set<std::string>(section_dot_key, str);
}

template<>
void ini_config::set(const std::string& section_dot_key, const double& value)
{
    auto str(std::to_string(value));
    this->set<std::string>(section_dot_key, str);
}

template<>
void ini_config::set(const std::string& section_dot_key, const float& value)
{
    auto str(std::to_string(value));
    this->set<std::string>(section_dot_key, str);
}

template<>
void ini_config::set(const std::string& section_dot_key, const long& value)
{
    auto str(std::to_string(value));
    this->set<std::string>(section_dot_key, str);
}

template<>
void ini_config::set(const std::string& section_dot_key, const long double& value)
{
    auto str(std::to_string(value));
    this->set<std::string>(section_dot_key, str);
}

template<>
void ini_config::set(const std::string& section_dot_key, const long long& value)
{
    auto str(std::to_string(value));
    this->set<std::string>(section_dot_key, str);
}

template<>
void ini_config::set(const std::string& section_dot_key, const unsigned long& value)
{
    auto str(std::to_string(value));
    this->set<std::string>(section_dot_key, str);
}

template<>
void ini_config::set(const std::string& section_dot_key, const unsigned long long& value)
{
    auto str(std::to_string(value));
    this->set<std::string>(section_dot_key, str);
}

void ini_config::write()
{
    if(needs_sync)
    {
        // convert to unordered_map of unordered_maps, one for each section
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;

        // iterate through cache
        for(const auto& line : this->cache)
        {
            // split into section,key
            auto parts(split(line.first, '.'));
            if(parts.size() > 2)
            {
                throw std::runtime_error("invalid key format, shouuld be section.key");
            }

            // create section if it does not exist, and insert key,value
            sections[parts[0]].insert(std::make_pair(parts[1], line.second.value));
        }

        std::ofstream stream(this->filename);

        // iterate through sections
        for(const auto& section : sections)
        {
            // output section header
            stream << '[' << section.first << "]\n";

            // iterate through keys
            for(const auto& key_pair : section.second)
            {
                stream << key_pair.first << " : " << key_pair.second << '\n';
            }
        }

        // set no need to sync to disk
        this->needs_sync = false;
    }
}

// return true if section.key exists in config
bool ini_config::exists(const std::string& section_dot_key) const
{
    // find key
    return this->cache.find(section_dot_key) != this->cache.end();
}

// add/remove an observer for a section.key
void ini_config::observe(const std::string& section_dot_key, observer* ob)
{
    // add observer
    this->find(section_dot_key)->second.observers.insert(ob);
}

void ini_config::unobserve(const std::string& section_dot_key, observer* ob)
{
    // remove observer
    this->find(section_dot_key)->second.observers.erase(ob);
}
