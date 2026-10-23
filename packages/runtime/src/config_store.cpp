#include <resonate/application/config_store.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "toml_text.h"

namespace resonate
{

struct ConfigStore::Table
{
    toml::table value;
};

namespace
{

struct VisitState
{
    void* user = nullptr;
    ConfigStore::ScalarVisitor visitor = nullptr;
    std::unordered_set<std::string>* seen = nullptr;
};

/* Nested tables flatten into dotted sections, which is the shape the store
   reads by; arrays and date-times are not settings and are left out. A path
   already visited by a higher layer is shadowed and is not reported again. */
void visitTable(const toml::table& table, const std::string& prefix, VisitState& state)
{
    for (const auto& [key, node] : table)
    {
        const std::string name(key.str());
        const std::string path = prefix.empty() ? name : prefix + "." + name;
        if (const toml::table* child = node.as_table())
        {
            visitTable(*child, path, state);
            continue;
        }
        if (!state.seen->insert(path).second)
        {
            continue;
        }

        std::string text;
        if (detail::scalarText(node, text))
        {
            state.visitor(state.user, prefix.c_str(), name.c_str(), text.c_str());
        }
    }
}

std::string dottedKey(const char* section, const char* key)
{
    std::string path = section != nullptr ? section : "";
    if (key != nullptr)
    {
        if (!path.empty())
        {
            path.push_back('.');
        }
        path.append(key);
    }
    return path;
}

std::unique_ptr<toml::table> parseFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return nullptr;
    }

    /* This toml++ version's parse_result is the table itself: false means the
       file did not parse, with no description to relay. */
    toml::parse_result result = toml::parse(in, path);
    if (!result)
    {
        std::cerr << "resonate: config " << path << ": does not parse\n";
        return nullptr;
    }
    return std::make_unique<toml::table>(std::move(result.table()));
}

} // namespace

ConfigStore::ConfigStore() = default;

ConfigStore::~ConfigStore() = default;

ConfigStore::ConfigStore(ConfigStore&& other) noexcept = default;

ConfigStore& ConfigStore::operator=(ConfigStore&& other) noexcept = default;

void ConfigStore::load(const std::string& path)
{
    layers_.clear();
    overrides_.clear();
    addFile(path);
}

void ConfigStore::addFile(const std::string& path)
{
    std::unique_ptr<toml::table> table = parseFile(path);
    if (table == nullptr)
    {
        return;
    }
    auto layer = std::make_unique<Table>();
    layer->value = std::move(*table);
    layers_.push_back(std::move(layer));
}

void ConfigStore::setOverride(const char* section, const char* key, std::string value)
{
    if (key == nullptr)
    {
        return;
    }
    overrides_[dottedKey(section, key)] = std::move(value);
}

void ConfigStore::clearOverrides()
{
    overrides_.clear();
}

std::optional<std::string> ConfigStore::value(const char* section, const char* key) const
{
    if (section == nullptr || key == nullptr)
    {
        return std::nullopt;
    }
    const std::string path = dottedKey(section, key);
    const auto override = overrides_.find(path);
    if (override != overrides_.end())
    {
        return override->second;
    }

    for (auto layer = layers_.rbegin(); layer != layers_.rend(); ++layer)
    {
        const toml::node* node = (*layer)->value.at_path(path).node();
        if (node == nullptr)
        {
            continue;
        }
        std::string text;
        return detail::scalarText(*node, text) ? std::optional<std::string>(std::move(text))
                                               : std::nullopt;
    }
    return std::nullopt;
}

void ConfigStore::visitScalars(void* user, ScalarVisitor visitor) const
{
    if (visitor == nullptr)
    {
        return;
    }

    /* Overrides first, then layers high to low; a key already seen is a value a
       higher layer replaced, and validating both would report the shadowed
       one. */
    std::unordered_set<std::string> seen;
    std::vector<std::string> names;
    names.reserve(overrides_.size());
    for (const auto& entry : overrides_)
    {
        names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    for (const std::string& path : names)
    {
        const auto separator = path.rfind('.');
        const std::string section =
            separator == std::string::npos ? std::string() : path.substr(0U, separator);
        const std::string key =
            separator == std::string::npos ? path : path.substr(separator + 1U);
        visitor(user, section.c_str(), key.c_str(), overrides_.at(path).c_str());
        seen.insert(path);
    }

    VisitState state{user, visitor, &seen};
    for (auto layer = layers_.rbegin(); layer != layers_.rend(); ++layer)
    {
        visitTable((*layer)->value, std::string(), state);
    }
}

bool ConfigStore::read(const char* section, const char* key, std::string& out) const
{
    const std::optional<std::string> text = value(section, key);
    if (!text.has_value())
    {
        return false;
    }
    out = *text;
    return true;
}

bool ConfigStore::readBool(const char* section, const char* key, bool fallback) const
{
    const std::optional<std::string> text = value(section, key);
    if (text == "true" || text == "1")
    {
        return true;
    }
    if (text == "false" || text == "0")
    {
        return false;
    }
    return fallback;
}

std::uint32_t ConfigStore::readUInt(const char* section, const char* key,
                                    std::uint32_t fallback) const
{
    const std::optional<std::string> text = value(section, key);
    if (!text.has_value() || text->empty() || (*text)[0] == '-' || (*text)[0] == '+')
    {
        return fallback;
    }

    const char* begin = text->c_str();
    char* end = nullptr;
    /* A 64-bit accumulator, so a value beyond 32 bits is seen as out of range
       instead of saturating at ULONG_MAX on a 32-bit long. */
    const unsigned long long parsed = std::strtoull(begin, &end, 10);
    if (end == begin || *end != '\0' || parsed > UINT32_MAX)
    {
        return fallback;
    }
    return static_cast<std::uint32_t>(parsed);
}

} // namespace resonate
