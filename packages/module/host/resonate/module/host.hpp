#ifndef RESONATE_MODULE_HOST_HPP
#define RESONATE_MODULE_HOST_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "resonate/core/allocator.h"
#include "resonate/module/abi.h"
#include "resonate/module/capability.h"
#include "resonate/module/manifest.hpp"
#include "resonate/module/stage.h"
#include "resonate/pal/library.h"

/*
 * Host side of the module system; not visible to a plugin. Lifecycle, in order:
 * discover, resolve, attachAll, runFrame, detachAll (the exact reverse of
 * attach).
 */

namespace resonate
{

struct ModuleRecord
{
    ModuleManifest manifest;
    std::string library_path;
    ResonateLibrary library;

    /* Published by the library and borrowed for as long as it stays loaded. */
    const ResonateModuleInfo* info = nullptr;
    const ResonateModuleVTable* vtable = nullptr;

    bool attached = false;
};

class ModuleGraph
{
  public:
    /* Orders the records so a module follows everything it depends on, whether
       that came from depends_on or from a required capability. Fails with
       RESONATE_E_INVALID on a cycle and RESONATE_E_MISSING on a depends_on name
       that no record carries. */
    ResonateStatus build(std::vector<ModuleRecord>& records);

    /* Attach order; detach is its reverse, and attach marks the records. */
    [[nodiscard]] const std::vector<ModuleRecord*>& order() const noexcept
    {
        return order_;
    }

    /* Why build() failed, in the words the host log should use. */
    [[nodiscard]] const std::string& error() const noexcept
    {
        return error_;
    }

  private:
    std::vector<ModuleRecord*> order_;
    std::string error_;
};

class ModuleHost
{
  public:
    static std::unique_ptr<ModuleHost> create(Allocator& allocator);

    ~ModuleHost();

    /* Loads every library in the directory, reads each one's identity, and pairs
       it with the manifest of the same id that sits beside it. A library that
       refuses to load is skipped and reported, not fatal. */
    ResonateStatus discover(const std::string& plugin_directory);

    /* Orders the graph and checks that every required capability has a provider. */
    ResonateStatus resolve();

    ResonateStatus attachAll();
    void detachAll();

    /* One frame: every stage in order, from EARLY_UPDATE through PRESENT. */
    void runFrame(float delta_seconds);

    [[nodiscard]] ResonateCapabilityRegistry* capabilities() const noexcept;
    [[nodiscard]] ResonateScheduler* scheduler() const noexcept;

    [[nodiscard]] const std::vector<ModuleRecord>& records() const noexcept
    {
        return records_;
    }

    /* Attach order as resolve() computed it; empty until resolve() has run. */
    [[nodiscard]] const std::vector<ModuleRecord*>& order() const noexcept;

    /* The single place plugin log output is filtered. */
    void setLogSink(void* user_data, ResonateLogFn sink);

    /* Opaque host state, defined in loader.cpp. Public only so the callbacks the
       host hands to a module can name it; nothing else can use it. */
    struct Impl;

  private:
    ModuleHost() = default;

    std::unique_ptr<Impl> impl_;
    std::vector<ModuleRecord> records_;
};

/* Returns RESONATE_E_INVALID, naming the offending field in the host log. */
ResonateStatus parseManifest(std::string_view json_text, ModuleManifest& out_manifest);

} // namespace resonate

#endif /* RESONATE_MODULE_HOST_HPP */
