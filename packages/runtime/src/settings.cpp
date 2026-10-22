#include <resonate/application/settings.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>

#include <resonate/application/config_store.h>

#include "toml_text.h"

namespace resonate
{
namespace
{

constexpr const char* kAllowedFields[] = {"key",     "type",       "default",    "range",
                                          "values",  "description", "deprecated"};

bool splitKey(const std::string& key, std::string& section, std::string& name)
{
    const std::size_t dot = key.find_last_of('.');
    if (dot == std::string::npos || dot == 0U || dot + 1U >= key.size())
    {
        return false;
    }
    section = key.substr(0U, dot);
    name = key.substr(dot + 1U);
    return true;
}

bool parseInt(const std::string& text, long long& out)
{
    if (text.empty())
    {
        return false;
    }
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0')
    {
        return false;
    }
    out = value;
    return true;
}

bool parseFloat(const std::string& text, double& out)
{
    if (text.empty())
    {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0')
    {
        return false;
    }
    out = value;
    return true;
}

std::string typeName(SettingType type)
{
    switch (type)
    {
    case SettingType::Int:
        return "int";
    case SettingType::Float:
        return "float";
    case SettingType::Bool:
        return "bool";
    case SettingType::String:
        return "string";
    case SettingType::Enum:
        return "enum";
    }
    return "unknown";
}

bool findType(const std::string& name, SettingType& type)
{
    if (name == "int")
    {
        type = SettingType::Int;
        return true;
    }
    if (name == "float")
    {
        type = SettingType::Float;
        return true;
    }
    if (name == "bool")
    {
        type = SettingType::Bool;
        return true;
    }
    if (name == "string")
    {
        type = SettingType::String;
        return true;
    }
    if (name == "enum")
    {
        type = SettingType::Enum;
        return true;
    }
    return false;
}

/* The value check validation and the default check share: one definition of
   what a declared type accepts. */
bool validValue(const SettingDecl& decl, const std::string& text, std::string& why)
{
    switch (decl.type)
    {
    case SettingType::Int:
    {
        long long value = 0;
        if (!parseInt(text, value))
        {
            why = "expected an integer";
            return false;
        }
        if (decl.bounded &&
            (static_cast<double>(value) < decl.minimum ||
             static_cast<double>(value) > decl.maximum))
        {
            why = "outside the declared range";
            return false;
        }
        return true;
    }
    case SettingType::Float:
    {
        double value = 0.0;
        if (!parseFloat(text, value))
        {
            why = "expected a number";
            return false;
        }
        if (decl.bounded && (value < decl.minimum || value > decl.maximum))
        {
            why = "outside the declared range";
            return false;
        }
        return true;
    }
    case SettingType::Bool:
        if (text != "true" && text != "false")
        {
            why = "expected a boolean";
            return false;
        }
        return true;
    case SettingType::String:
        return true;
    case SettingType::Enum:
        for (const std::string& value : decl.values)
        {
            if (value == text)
            {
                return true;
            }
        }
        why = "expected one of";
        for (const std::string& value : decl.values)
        {
            why += " '" + value + "'";
        }
        return false;
    }
    why = "unknown type";
    return false;
}

struct ValidationVisit
{
    const SettingsSchema* schema = nullptr;
    std::vector<std::string>* warnings = nullptr;
    std::string* error = nullptr;
    bool failed = false;
};

void visitSetting(void* user, const char* section, const char* key, const char* value)
{
    auto* visit = static_cast<ValidationVisit*>(user);
    const std::string full =
        (section != nullptr && *section != '\0') ? std::string(section) + "." + key : key;
    const SettingDecl* decl = visit->schema->find(full.c_str());
    if (decl == nullptr)
    {
        visit->warnings->push_back("unknown setting '" + full + "'");
        return;
    }
    std::string why;
    if (!validValue(*decl, value, why) && !visit->failed)
    {
        *visit->error = "setting '" + full + "': " + why;
        visit->failed = true;
    }
}

} // namespace

bool SettingsSchema::load(const std::string& path, std::string& error)
{
    error.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot read " + path;
        return false;
    }

    const toml::parse_result result = toml::parse(in, path);
    if (!result)
    {
        error = path + ": does not parse";
        return false;
    }

    const toml::array* settings = result.table()["setting"].as_array();
    if (settings == nullptr)
    {
        error = path + ": no [[setting]] declarations";
        return false;
    }

    std::vector<SettingDecl> parsed;
    parsed.reserve(settings->size());
    for (std::size_t index = 0; index < settings->size(); ++index)
    {
        const toml::table* entry = (*settings)[index].as_table();
        const std::string where = "setting #" + std::to_string(index + 1U);
        if (entry == nullptr)
        {
            error = where + " is not a table";
            return false;
        }

        for (const auto& [field, node] : *entry)
        {
            const std::string name(field.str());
            bool allowed = false;
            for (const char* candidate : kAllowedFields)
            {
                if (name == candidate)
                {
                    allowed = true;
                    break;
                }
            }
            if (!allowed)
            {
                error = where + ": unknown field '" + name + "'";
                return false;
            }
            (void)node;
        }

        SettingDecl decl;
        const toml::node* key_node = entry->get("key");
        const std::optional<std::string> key_text =
            key_node != nullptr && key_node->is_string() ? key_node->value<std::string>()
                                                         : std::nullopt;
        if (!key_text.has_value())
        {
            error = where + ": 'key' must be a string";
            return false;
        }
        std::string section;
        std::string name;
        if (!splitKey(*key_text, section, name))
        {
            error = where + ": '" + *key_text + "' is not a dotted key";
            return false;
        }
        decl.key = *key_text;

        const toml::node* type_node = entry->get("type");
        const std::optional<std::string> type_text =
            type_node != nullptr && type_node->is_string() ? type_node->value<std::string>()
                                                           : std::nullopt;
        if (!type_text.has_value() || !findType(*type_text, decl.type))
        {
            error = where + " ('" + decl.key + "'): 'type' must be int, float, bool, string or enum";
            return false;
        }

        if (const toml::node* range = entry->get("range"))
        {
            if (decl.type != SettingType::Int && decl.type != SettingType::Float)
            {
                error = where + " ('" + decl.key + "'): 'range' only applies to int and float";
                return false;
            }
            const toml::array* bounds = range->as_array();
            if (bounds == nullptr || bounds->size() != 2U)
            {
                error = where + " ('" + decl.key + "'): 'range' needs two numbers";
                return false;
            }
            double values[2] = {0.0, 0.0};
            for (std::size_t side = 0; side < 2U; ++side)
            {
                const toml::node& bound = (*bounds)[side];
                if (bound.is_integer())
                {
                    values[side] = static_cast<double>(*bound.value<std::int64_t>());
                }
                else if (bound.is_floating_point())
                {
                    values[side] = *bound.value<double>();
                }
                else
                {
                    error = where + " ('" + decl.key + "'): 'range' needs numbers";
                    return false;
                }
            }
            if (values[0] > values[1])
            {
                error = where + " ('" + decl.key + "'): 'range' is inverted";
                return false;
            }
            decl.bounded = true;
            decl.minimum = values[0];
            decl.maximum = values[1];
        }

        if (const toml::node* values = entry->get("values"))
        {
            if (decl.type != SettingType::Enum)
            {
                error = where + " ('" + decl.key + "'): 'values' only applies to enum";
                return false;
            }
            const toml::array* members = values->as_array();
            if (members == nullptr || members->empty())
            {
                error = where + " ('" + decl.key + "'): 'values' needs enum members";
                return false;
            }
            for (const toml::node& member : *members)
            {
                if (!member.is_string())
                {
                    error = where + " ('" + decl.key + "'): enum members are strings";
                    return false;
                }
                decl.values.push_back(*member.value<std::string>());
            }
        }
        if (decl.type == SettingType::Enum && decl.values.empty())
        {
            error = where + " ('" + decl.key + "'): an enum needs 'values'";
            return false;
        }

        const toml::node* default_node = entry->get("default");
        if (default_node == nullptr || !detail::scalarText(*default_node, decl.default_value))
        {
            error = where + " ('" + decl.key + "'): 'default' must be a scalar";
            return false;
        }
        std::string why;
        if (!validValue(decl, decl.default_value, why))
        {
            error = where + " ('" + decl.key + "'): the default is invalid: " + why;
            return false;
        }

        if (const toml::node* description = entry->get("description"))
        {
            if (!description->is_string())
            {
                error = where + " ('" + decl.key + "'): 'description' must be a string";
                return false;
            }
            decl.description = *description->value<std::string>();
        }

        for (const SettingDecl& previous : parsed)
        {
            if (previous.key == decl.key)
            {
                error = where + ": '" + decl.key + "' is declared twice";
                return false;
            }
        }
        parsed.push_back(std::move(decl));
    }

    declarations_ = std::move(parsed);
    return true;
}

const SettingDecl* SettingsSchema::find(const char* key) const noexcept
{
    if (key == nullptr)
    {
        return nullptr;
    }
    for (const SettingDecl& decl : declarations_)
    {
        if (std::strcmp(decl.key.c_str(), key) == 0)
        {
            return &decl;
        }
    }
    return nullptr;
}

bool SettingsSchema::validate(const ConfigStore& store, std::vector<std::string>& warnings,
                              std::string& error) const
{
    error.clear();
    ValidationVisit visit{this, &warnings, &error, false};
    store.visitScalars(&visit, &visitSetting);
    return !visit.failed;
}

std::string SettingsSchema::valueText(const ConfigStore& store, const SettingDecl& decl) const
{
    std::string section;
    std::string name;
    if (!splitKey(decl.key, section, name))
    {
        return decl.default_value;
    }
    const std::optional<std::string> text = store.value(section.c_str(), name.c_str());
    return text.has_value() ? *text : decl.default_value;
}

std::uint32_t SettingsSchema::readUInt(const ConfigStore& store, const char* key,
                                       std::uint32_t fallback) const
{
    const SettingDecl* decl = find(key);
    if (decl == nullptr || decl->type != SettingType::Int)
    {
        return fallback;
    }
    long long value = 0;
    if (!parseInt(valueText(store, *decl), value) || value < 0 ||
        value > static_cast<long long>(std::numeric_limits<std::uint32_t>::max()))
    {
        return fallback;
    }
    return static_cast<std::uint32_t>(value);
}

float SettingsSchema::readFloat(const ConfigStore& store, const char* key, float fallback) const
{
    const SettingDecl* decl = find(key);
    if (decl == nullptr || decl->type != SettingType::Float)
    {
        return fallback;
    }
    double value = 0.0;
    return parseFloat(valueText(store, *decl), value) ? static_cast<float>(value) : fallback;
}

bool SettingsSchema::readBool(const ConfigStore& store, const char* key, bool fallback) const
{
    const SettingDecl* decl = find(key);
    if (decl == nullptr || decl->type != SettingType::Bool)
    {
        return fallback;
    }
    const std::string text = valueText(store, *decl);
    if (text == "true")
    {
        return true;
    }
    if (text == "false")
    {
        return false;
    }
    return fallback;
}

std::string SettingsSchema::readString(const ConfigStore& store, const char* key,
                                       const std::string& fallback) const
{
    const SettingDecl* decl = find(key);
    if (decl == nullptr ||
        (decl->type != SettingType::String && decl->type != SettingType::Enum))
    {
        return fallback;
    }
    return valueText(store, *decl);
}

} // namespace resonate
