#ifndef RESONATE_APPLICATION_CONFIG_STORE_H
#define RESONATE_APPLICATION_CONFIG_STORE_H

/* The run's configuration file, loaded once at startup: TOML, with
   reverse-DNS keys — section "resonate.render", key "vsync" is
   `[resonate.render] vsync` in the file. The store answers the host API's
   read_config for every module, so each one reads its own namespace and the
   host reads nothing it does not own. The store reads; it does not validate
   against declarations, watch for changes or reload.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <resonate/module/host.hpp>

namespace resonate
{

class ConfigStore final : public ConfigSource
{
  public:
    ConfigStore();
    ~ConfigStore() override;

    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    ConfigStore(ConfigStore&& other) noexcept;
    ConfigStore& operator=(ConfigStore&& other) noexcept;

    /* Reads the file into the store. A file that cannot be opened leaves the
       store empty — configuration is optional — but one that cannot be parsed
       is reported on stderr and also left empty: a typo silently ignored would
       be verification that did not happen. */
    void load(const std::string& path);

    /* The store's own read: the scalar's text form, or nullopt when the key is
       unset or names a table or an array. */
    [[nodiscard]] std::optional<std::string> value(const char* section, const char* key) const;

    /* ConfigSource. */
    bool read(const char* section, const char* key, std::string& out) const override;

    /* Typed reads over value(), with the same exact-spelling rule a module's
       typed reads follow. */
    [[nodiscard]] bool readBool(const char* section, const char* key, bool fallback) const;
    [[nodiscard]] std::uint32_t readUInt(const char* section, const char* key,
                                         std::uint32_t fallback) const;

  private:
    struct Table;
    std::unique_ptr<Table> table_;
};

} // namespace resonate

#endif /* RESONATE_APPLICATION_CONFIG_STORE_H */
