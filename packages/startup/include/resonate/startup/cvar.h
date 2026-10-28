#ifndef RESONATE_STARTUP_CVAR_H
#define RESONATE_STARTUP_CVAR_H

/*
 * The run's CVars: settings keys bound to live appliers, so a console command
 * reaches the running system instead of only the config file. The host binds
 * the engine's own keys at startup; an application adds its own the same way
 * and reaches the whole set through the HostExtension's pointer.
 *
 * The ConfigStore is not touched: a bound key answers for the live value, and
 * writing the store back (persistence) comes with the config hot-reload slice.
 */

#include <string>
#include <vector>

namespace resonate
{

class SettingsSchema;

namespace startup
{

/* One bound key. Function pointers and a context: plain data a C module could
   bind through the same shape. */
struct CVarBinding
{
    std::string key;
    void* context = nullptr;
    /* Applies the (schema-validated) text to the live system. */
    bool (*apply)(void* context, const std::string& value, std::string& error) = nullptr;
    /* The value the layer last set or started with. */
    std::string (*read)(void* context) = nullptr;
};

class CVarHost
{
  public:
    /* Validates against the schema before the applier runs; a run without a
       schema skips the check and trusts the applier. The last binding for a
       key wins. */
    void setSchema(const SettingsSchema* schema)
    {
        schema_ = schema;
    }

    void bind(CVarBinding binding);

    /* False with `error` when the key is unknown, the value fails its
       declaration, or the applier refuses. */
    bool apply(const std::string& key, const std::string& value, std::string& error);

    /* The bound value for `key`; false when the key is unknown. */
    [[nodiscard]] bool read(const std::string& key, std::string& out) const;

    /* The bound keys containing `pattern` (an empty pattern matches all), in
       binding order. */
    [[nodiscard]] std::vector<std::string> find(const std::string& pattern) const;

  private:
    const SettingsSchema* schema_ = nullptr;
    std::vector<CVarBinding> bindings_;
};

} // namespace startup
} // namespace resonate

#endif /* RESONATE_STARTUP_CVAR_H */
