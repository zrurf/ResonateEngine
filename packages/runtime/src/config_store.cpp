#include <resonate/application/config_store.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "toml_text.h"

namespace resonate
{

struct ConfigStore::Table
{
    toml::table value;
};

namespace
{

/* Nested tables flatten into dotted sections, which is the shape the store
   reads by; arrays and date-times are not settings and are left out. */
void visitTable(const toml::table& table, const std::string& prefix, void* user,
                ConfigStore::ScalarVisitor visitor)
{
    for (const auto& [key, node] : table)
    {
        const std::string name(key.str());
        const std::string path = prefix.empty() ? name : prefix + "." + name;
        if (const toml::table* child = node.as_table())
        {
            visitTable(*child, path, user, visitor);
            continue;
        }

        std::string text;
        if (detail::scalarText(node, text))
        {
            visitor(user, prefix.c_str(), name.c_str(), text.c_str());
        }
    }
}

} // namespace

ConfigStore::ConfigStore() = default;

ConfigStore::~ConfigStore() = default;

ConfigStore::ConfigStore(ConfigStore&& other) noexcept = default;

ConfigStore& ConfigStore::operator=(ConfigStore&& other) noexcept = default;

void ConfigStore::load(const std::string& path)
{
    table_ = nullptr;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return;
    }

    /* This toml++ version's parse_result is the table itself: false means the
       file did not parse, with no description to relay. */
    const toml::parse_result result = toml::parse(in, path);
    if (!result)
    {
        std::cerr << "resonate: config " << path << ": does not parse\n";
        return;
    }
    table_ = std::make_unique<Table>(Table{std::move(result.table())});
}

std::optional<std::string> ConfigStore::value(const char* section, const char* key) const
{
    if (table_ == nullptr || section == nullptr || key == nullptr)
    {
        return std::nullopt;
    }

    const toml::node* node = table_->value.at_path(std::string(section) + '.' + key).node();
    if (node == nullptr)
    {
        return std::nullopt;
    }

    std::string text;
    return detail::scalarText(*node, text) ? std::optional<std::string>(std::move(text))
                                           : std::nullopt;
}

void ConfigStore::visitScalars(void* user, ScalarVisitor visitor) const
{
    if (table_ == nullptr || visitor == nullptr)
    {
        return;
    }
    visitTable(table_->value, std::string(), user, visitor);
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
    const unsigned long parsed = std::strtoul(begin, &end, 10);
    if (end == begin || *end != '\0' || parsed > UINT32_MAX)
    {
        return fallback;
    }
    return static_cast<std::uint32_t>(parsed);
}

} // namespace resonate
