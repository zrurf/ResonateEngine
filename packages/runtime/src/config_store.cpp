#include <resonate/application/config_store.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <resonate/pal/watch.h>

#include "toml_text.h"

namespace resonate
{

struct ConfigStore::Table
{
    toml::table value;
};

struct ConfigStore::Watch
{
    ResonateWatchHandle handle{};
    std::string path;

    ~Watch()
    {
        resonate_pal_watch_close(&handle);
    }
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

/* One layer file into `out`. A missing file is an empty layer (an optional
   layer that is not there yet); false means the file exists but does not
   parse, with the reason. */
bool parseLayer(const std::string& path, toml::table& out, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        out = toml::table{};
        return true;
    }

    /* This toml++ version's parse_result is the table itself: false means the
       file did not parse, with no description to relay. */
    toml::parse_result result = toml::parse(in, path);
    if (!result)
    {
        error = path + ": does not parse";
        return false;
    }
    out = std::move(result.table());
    return true;
}

/* The store's effective scalars as dotted key -> text, through the same
   shadowing visitScalars applies. */
void collectEffective(const ConfigStore& store,
                      std::unordered_map<std::string, std::string>& out)
{
    struct Collect
    {
        static void visit(void* user, const char* section, const char* key, const char* value)
        {
            auto* map = static_cast<std::unordered_map<std::string, std::string>*>(user);
            std::string path = section != nullptr ? section : "";
            if (key != nullptr)
            {
                if (!path.empty())
                {
                    path.push_back('.');
                }
                path.append(key);
            }
            (*map)[std::move(path)] = value != nullptr ? value : "";
        }
    };
    store.visitScalars(&out, &Collect::visit);
}

/* The layer timestamps, as text: a removed file reads empty, which differs
   from any present file. */
std::string stampOf(const std::string& path)
{
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return {};
    }
    return std::to_string(static_cast<std::uint64_t>(stamp.time_since_epoch().count()));
}

} // namespace

ConfigStore::ConfigStore() = default;

ConfigStore::~ConfigStore() = default;

ConfigStore::ConfigStore(ConfigStore&& other) noexcept = default;

ConfigStore& ConfigStore::operator=(ConfigStore&& other) noexcept = default;

void ConfigStore::load(const std::string& path)
{
    closeWatches();
    layers_.clear();
    layer_paths_.clear();
    overrides_.clear();
    addFile(path);
    captureStamps();
}

void ConfigStore::addFile(const std::string& path)
{
    /* The path is recorded even when the file is missing or broken: watching
       it is how a file that appears later is ever read. */
    layer_paths_.push_back(path);
    toml::table value;
    std::string error;
    if (!parseLayer(path, value, error))
    {
        std::cerr << "resonate: config " << error << "\n";
    }
    auto layer = std::make_unique<Table>();
    layer->value = std::move(value);
    layers_.push_back(std::move(layer));
}

void ConfigStore::setChangeSink(void* user, ChangeSinkFn sink)
{
    change_user_ = user;
    change_ = sink;
}

void ConfigStore::setValidation(void* user, ValidationFn validation)
{
    validation_user_ = user;
    validation_ = validation;
}

void ConfigStore::setMessageSink(void* user, MessageFn sink)
{
    message_user_ = user;
    message_ = sink;
}

bool ConfigStore::watchFiles()
{
    closeWatches();
    watches_.clear();
    bool any = false;
    for (const std::string& path : layer_paths_)
    {
        auto watch = std::make_unique<Watch>();
        watch->path = path;
        const ResonatePalStatus status = resonate_pal_watch_create(&watch->handle, path.c_str());
        if (status != RESONATE_PAL_OK)
        {
            say("config: cannot watch " + path + ": " + std::to_string(status));
            continue;
        }
        watches_.push_back(std::move(watch));
        any = true;
    }
    captureStamps();
    return any;
}

void ConfigStore::poll()
{
    if (watches_.empty())
    {
        return;
    }
    bool signaled = false;
    for (const std::unique_ptr<Watch>& watch : watches_)
    {
        if (resonate_pal_watch_poll(&watch->handle) > 0)
        {
            signaled = true;
        }
    }
    if (!signaled)
    {
        return;
    }

    /* The notification names the directory: only a layer whose timestamp
       moved is a reason to reload. */
    bool changed = false;
    for (std::size_t index = 0; index < layer_paths_.size() && index < stamps_.size(); ++index)
    {
        if (stampOf(layer_paths_[index]) != stamps_[index])
        {
            changed = true;
            break;
        }
    }
    if (!changed)
    {
        return;
    }

    /* Reparse the whole stack: layer order is the merge rule, and a partial
       rebuild would have to reimplement it. */
    std::vector<std::unique_ptr<Table>> fresh;
    fresh.reserve(layer_paths_.size());
    for (const std::string& path : layer_paths_)
    {
        toml::table value;
        std::string error;
        if (!parseLayer(path, value, error))
        {
            captureStamps();
            say("config: kept the previous configuration: " + error);
            return;
        }
        auto layer = std::make_unique<Table>();
        layer->value = std::move(value);
        fresh.push_back(std::move(layer));
    }

    ConfigStore probe;
    probe.overrides_ = overrides_;
    probe.layers_ = std::move(fresh);

    /* Validate every new scalar before anything is committed. */
    std::string validation_error;
    if (validation_ != nullptr)
    {
        struct Check
        {
            void* user = nullptr;
            ValidationFn validation = nullptr;
            std::string* error = nullptr;
            bool ok = true;

            static void visit(void* data, const char* section, const char* key, const char* value)
            {
                auto* check = static_cast<Check*>(data);
                if (!check->ok || check->validation == nullptr)
                {
                    return;
                }
                std::string reason;
                if (!check->validation(check->user, section, key, value, reason))
                {
                    check->ok = false;
                    *check->error = reason;
                }
            }
        };
        Check check{validation_user_, validation_, &validation_error, true};
        probe.visitScalars(&check, &Check::visit);
        if (!check.ok)
        {
            captureStamps();
            say("config: kept the previous configuration: " + validation_error);
            return;
        }
    }

    /* Diff the effective values (the override layer included), then commit and
       notify: a consumer that reacts reads the new store. */
    std::unordered_map<std::string, std::string> before;
    std::unordered_map<std::string, std::string> after;
    collectEffective(*this, before);
    collectEffective(probe, after);
    layers_ = std::move(probe.layers_);
    captureStamps();

    std::vector<std::string> keys;
    keys.reserve(before.size() + after.size());
    for (const auto& [key, value] : before)
    {
        keys.push_back(key);
    }
    for (const auto& [key, value] : after)
    {
        if (before.find(key) == before.end())
        {
            keys.push_back(key);
        }
    }
    std::sort(keys.begin(), keys.end());

    std::size_t changes = 0;
    for (const std::string& path : keys)
    {
        const auto old_entry = before.find(path);
        const auto new_entry = after.find(path);
        const std::string old_text = old_entry != before.end() ? old_entry->second : std::string();
        const std::string new_text = new_entry != after.end() ? new_entry->second : std::string();
        if (old_text == new_text &&
            (old_entry != before.end()) == (new_entry != after.end()))
        {
            continue;
        }
        ++changes;
        if (change_ == nullptr)
        {
            continue;
        }
        /* The event's value is empty for a key the file no longer sets; the
           reader falls back to the declaration default, like value() does. */
        const std::size_t separator = path.rfind('.');
        const std::string section =
            separator == std::string::npos ? std::string() : path.substr(0U, separator);
        const std::string key =
            separator == std::string::npos ? path : path.substr(separator + 1U);
        change_(change_user_, section.c_str(), key.c_str(), new_text.c_str());
    }
    say("config: reloaded, " + std::to_string(changes) + " value(s) changed");
}

void ConfigStore::closeWatches()
{
    for (std::unique_ptr<Watch>& watch : watches_)
    {
        resonate_pal_watch_close(&watch->handle);
    }
    watches_.clear();
}

void ConfigStore::captureStamps()
{
    stamps_.clear();
    stamps_.reserve(layer_paths_.size());
    for (const std::string& path : layer_paths_)
    {
        stamps_.push_back(stampOf(path));
    }
}

void ConfigStore::say(const std::string& text) const
{
    if (message_ != nullptr)
    {
        message_(message_user_, text.c_str());
        return;
    }
    std::cerr << "resonate: " << text << "\n";
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
