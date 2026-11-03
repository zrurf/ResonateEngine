#ifndef RESONATE_APPLICATION_CONFIG_STORE_H
#define RESONATE_APPLICATION_CONFIG_STORE_H

/* The run's configuration: TOML layers with reverse-DNS keys — section
   "resonate.render", key "vsync" is `[resonate.render] vsync` in the file. The
   store answers the host API's read_config for every module, so each one reads
   its own namespace and the host reads nothing it does not own. A later layer
   overrides an earlier one per key; the override layer (a console's CVar)
   sits above every file. The store watches its files and reloads them: a
   reload validates through the caller's sink, keeps the previous stack when a
   file or a value is bad, and then reports one change per key whose effective
   value moved.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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

    /* Replaces the whole stack and the override layer with one file. A file
       that cannot be opened leaves the store empty — configuration is optional
       — but one that cannot be parsed is reported on stderr and also left
       empty: a typo silently ignored would be verification that did not
       happen. */
    void load(const std::string& path);

    /* Adds a sparse layer above the layers already loaded; a later layer
       overrides an earlier one per key. A file that cannot be parsed keeps the
       stack it had. */
    void addFile(const std::string& path);

    /* The CVar layer, above every file. It is read through the same value()
       path as the files, so a module cannot tell an override from a file. */
    void setOverride(const char* section, const char* key, std::string value);
    void clearOverrides();

    /* The store's own read: the scalar's text form, or nullopt when the key is
       unset or names a table or an array. */
    [[nodiscard]] std::optional<std::string> value(const char* section, const char* key) const;

    /* ConfigSource. */
    bool read(const char* section, const char* key, std::string& out) const override;

    /* Visits every scalar as its text form: nested tables flatten into dotted
       sections, which is the same shape value() reads. Arrays and date-times
       are not settings and are skipped. */
    using ScalarVisitor = void (*)(void* user, const char* section, const char* key,
                                   const char* value);
    void visitScalars(void* user, ScalarVisitor visitor) const;

    /* Typed reads over value(), with the same exact-spelling rule a module's
       typed reads follow. */
    [[nodiscard]] bool readBool(const char* section, const char* key, bool fallback) const;
    [[nodiscard]] std::uint32_t readUInt(const char* section, const char* key,
                                         std::uint32_t fallback) const;

    /* --- change notification and hot reload --- */

    /* One per-key notification, emitted after a reload whose effective value
       for the key differs; `value` is the new effective text, empty when the
       key lost its value (the declaration default applies). */
    using ChangeSinkFn = void (*)(void* user, const char* section, const char* key,
                                  const char* value);
    /* Checks one new scalar before the reload commits. False names the reason
       and keeps the previous configuration. */
    using ValidationFn = bool (*)(void* user, const char* section, const char* key,
                                  const char* value, std::string& error);
    /* Reload notes and failures, for the run's log. */
    using MessageFn = void (*)(void* user, const char* message);

    void setChangeSink(void* user, ChangeSinkFn sink);
    void setValidation(void* user, ValidationFn validation);
    void setMessageSink(void* user, MessageFn sink);

    /* Starts watching every layer file's directory (a missing file included:
       creating it is a change). Call after the layers are loaded. */
    bool watchFiles();

    /* Cheap per-frame check: a pending notification and a changed timestamp,
       then a full reparse, validation and one change event per key whose
       effective value differs. A failed parse or validation keeps the
       previous stack and reports it. */
    void poll();

  private:
    struct Table;
    struct Watch;
    std::vector<std::unique_ptr<Table>> layers_;
    std::vector<std::string> layer_paths_;
    std::unordered_map<std::string, std::string> overrides_;
    std::vector<std::unique_ptr<Watch>> watches_;
    std::vector<std::string> stamps_;
    void* change_user_ = nullptr;
    ChangeSinkFn change_ = nullptr;
    void* validation_user_ = nullptr;
    ValidationFn validation_ = nullptr;
    void* message_user_ = nullptr;
    MessageFn message_ = nullptr;

    void closeWatches();
    void captureStamps();
    void say(const std::string& text) const;
};

} // namespace resonate

#endif /* RESONATE_APPLICATION_CONFIG_STORE_H */
