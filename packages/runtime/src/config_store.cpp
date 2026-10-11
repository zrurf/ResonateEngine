#include <resonate/application/config_store.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <type_traits>

/* A bad config file is a stderr line, not an exception unwinding the run. The
   macro has to precede the include, and every TU that includes toml++ must
   agree on it — loader.cpp sets the same one for manifest parsing. */
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

namespace resonate
{

struct ConfigStore::Table
{
    toml::table value;
};

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

    std::optional<std::string> out;
    node->visit(
        [&](auto&& value)
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, toml::value<std::string>>)
            {
                out = value.get();
            }
            else if constexpr (std::is_same_v<Value, toml::value<std::int64_t>>)
            {
                out = std::to_string(value.get());
            }
            else if constexpr (std::is_same_v<Value, toml::value<double>>)
            {
                out = std::to_string(value.get());
            }
            else if constexpr (std::is_same_v<Value, toml::value<bool>>)
            {
                out = value.get() ? std::string("true") : std::string("false");
            }
        });
    return out;
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
