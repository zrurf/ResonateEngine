#ifndef RESONATE_APPLICATION_SETTINGS_H
#define RESONATE_APPLICATION_SETTINGS_H

/* The project's settings contract: every key the engine reads, with its type,
   range, default and description. The run's config file is checked against it
   before anything consumes a value — an unknown key is a warning (a stale or
   misspelled name), a value of the wrong type or outside its range is refused,
   because silently falling back would be verification that did not happen.

   The declaration file is TOML with one [[setting]] table per key. Layering,
   change events and hot reload are later slices; this one settles the contract,
   its validation and the typed reads. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace resonate
{

class ConfigStore;

enum class SettingType
{
    Int,
    Float,
    Bool,
    String,
    Enum,
};

struct SettingDecl
{
    /* Dotted reverse-DNS key, e.g. "resonate.window.width". */
    std::string key;
    SettingType type = SettingType::String;
    /* Text form, parsed exactly like a config value. */
    std::string default_value;
    bool bounded = false;
    double minimum = 0.0;
    double maximum = 0.0;
    /* Enum members; empty for every other type. */
    std::vector<std::string> values;
    std::string description;
};

class SettingsSchema final
{
  public:
    SettingsSchema() = default;
    ~SettingsSchema() = default;

    SettingsSchema(SettingsSchema&&) noexcept = default;
    SettingsSchema& operator=(SettingsSchema&&) noexcept = default;

    SettingsSchema(const SettingsSchema&) = delete;
    SettingsSchema& operator=(const SettingsSchema&) = delete;

    /* Reads the [[setting]] declarations from `path`. A malformed declaration
       or a duplicate key fails the load and leaves the schema unchanged. */
    bool load(const std::string& path, std::string& error);

    [[nodiscard]] std::size_t size() const noexcept
    {
        return declarations_.size();
    }

    [[nodiscard]] const SettingDecl* find(const char* key) const noexcept;

    /* Checks the store's scalar keys against the declarations. Each unknown key
       is appended to `warnings`; the first bad value stops the check and is
       reported through `error`. Arrays are not settings and are not visited. */
    bool validate(const ConfigStore& store, std::vector<std::string>& warnings,
                  std::string& error) const;

    /* Typed reads: the config value when set, the declared default otherwise,
       and `fallback` when the key has no declaration of that type. Values have
       passed validate() already. */
    [[nodiscard]] std::uint32_t readUInt(const ConfigStore& store, const char* key,
                                         std::uint32_t fallback) const;
    [[nodiscard]] float readFloat(const ConfigStore& store, const char* key, float fallback) const;
    [[nodiscard]] bool readBool(const ConfigStore& store, const char* key, bool fallback) const;
    [[nodiscard]] std::string readString(const ConfigStore& store, const char* key,
                                         const std::string& fallback) const;

  private:
    [[nodiscard]] std::string valueText(const ConfigStore& store, const SettingDecl& decl) const;

    std::vector<SettingDecl> declarations_;
};

} // namespace resonate

#endif /* RESONATE_APPLICATION_SETTINGS_H */
