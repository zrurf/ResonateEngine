#ifndef RESONATE_MODULE_HOST_HPP
#define RESONATE_MODULE_HOST_HPP

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "resonate/core/allocator.h"
#include "resonate/core/job.h"
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

namespace ecs
{
class World;
}

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

    /* --- the frame's ECS and its sync points --- */

    /* The world the run's systems record into, borrowed for as long as it is
       set. Called before modules attach, so a world-aware registration finds
       it; null means this run has no ECS and such a registration is refused.
       The world must outlive the host: the command buffers the host holds for
       its systems release their reservations through it on the way out. */
    void setWorld(ecs::World* world) noexcept;
    [[nodiscard]] ecs::World* world() const noexcept;

    /* Registers one system on the schedule, filling the world/commands handles
       a world-aware descriptor asks for: the run's world and a command buffer
       of that system's own. The module-facing system_add funnels through here,
       so a plugin's systems and the engine's own take the same path. Refused
       with RESONATE_E_STATE when the descriptor is world-aware and setWorld has
       not been called. */
    ResonateStatus addSystem(const ResonateSystemDesc& desc);

    /* Runs one stage's graph. The host's frame composes these around the sync
       points; a caller driving stages itself has to play them itself too. */
    void runStage(ResonateStage stage, float delta_seconds);

    /* One frame: every stage in order, from EARLY_UPDATE through PRESENT, with
       the two sync points — between UPDATE and PHYSICS, and between PHYSICS and
       LATE — playing the recorded command buffers. What is recorded after the
       second one is reported and dropped: nothing plays again this frame. */
    void runFrame(float delta_seconds);

    /* The sync point: plays every non-empty command buffer in registration
       order, so what one stage recorded exists before the next runs. A played
       buffer is empty, which is what keeps playback to once per frame. Every
       play goes through World::play, so a parallel scope open at the sync point
       refuses it and the buffer keeps its commands for the next one. */
    void playRecordedCommands();

    [[nodiscard]] ResonateCapabilityRegistry* capabilities() const noexcept;
    [[nodiscard]] ResonateScheduler* scheduler() const noexcept;

    /* The pool the schedule runs on, borrowed and owned by the scheduler. It is
       the one execution substrate of the run: engine-side C++ (the ECS's
       parallel iteration, later the assets' decode jobs) submits its own work
       here, and `requestWorkerCount` is what a runtime config tunes.

       Null only if the pool could not be created, in which case stages run in
       registration order and there is nothing to submit to. */
    [[nodiscard]] JobSystem* jobs() const noexcept;

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

/* The pool a schedule runs on, or null for one that could not create it. Host
   side only: the C surface the scheduler is called through has no type for it,
   and a plugin reaches work through its own system registrations. */
JobSystem* jobsOf(ResonateScheduler* scheduler);

} // namespace resonate

#endif /* RESONATE_MODULE_HOST_HPP */
