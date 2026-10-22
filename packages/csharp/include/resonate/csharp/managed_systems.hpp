#ifndef RESONATE_CSHARP_MANAGED_SYSTEMS_H
#define RESONATE_CSHARP_MANAGED_SYSTEMS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <resonate/csharp/bindings.h>
#include <resonate/csharp/dispatch.h>
#include <resonate/csharp/host.hpp>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/gameplay/action_map.h>
#include <resonate/module/stage.h>

namespace resonate::csharp
{

/* The unmanaged entry the registration generator emits; the plain Register it
   also emits serves the reflective patch path. */
inline constexpr const char* kRegisterEntry = "RegisterUnmanaged";

/* A managed assembly's systems, one engine system per stage it declares. The
   assembly registers its entries into per-stage tables when it loads; the
   frame loop invokes them through the tables with the generated frame context,
   so no delegate reaches the engine. */
class ManagedSystems
{
public:
    /* The stage declarations the host can schedule, and the thunk for each.
       The descriptors borrow these names and thunks for the run's lifetime. */
    struct StageEntry
    {
        ResonateStage stage;
        const char* name;
        void (*run)(void*, ecs::World&, ecs::CommandBuffer&, float);
    };
    static constexpr std::size_t kStageEntryCount = 6U;
    static const StageEntry* stageEntries();

    ManagedSystems() = default;
    ~ManagedSystems();

    ManagedSystems(const ManagedSystems&) = delete;
    ManagedSystems& operator=(const ManagedSystems&) = delete;

    /* Starts the runtime, loads the assembly and calls its registration entry.
       `type_name` is the assembly-qualified type holding the entry. */
    bool load(const std::string& runtime_config, const std::string& assembly,
              const std::string& type_name, std::string& error);

    void unload();

    /* Native state the managed systems may reference through the context. */
    void set_user(void* user);

    /* The frame's input actions; the host API answers read_action from it.
       Null (the default) makes every action read fail. */
    void set_actions(const gameplay::ActionMap* actions);

    /* Installs a patch from `patch_assembly` through a managed bootstrap: the
       patch's Register replaces the slots it names, and every displaced entry
       is remembered so unload restores it. One patch at a time. */
    bool load_patch(const std::string& bootstrap_assembly, const std::string& bootstrap_type,
                    const std::string& patch_assembly, const std::string& entry_type,
                    std::string& error);

    /* Restores the replaced slots (the patch's code is about to go), then
       unloads its context. */
    bool unload_patch(std::string& error);

    bool patched() const
    {
        return !patch_frames_.empty();
    }

    /* The registration callbacks a managed assembly and a patch reach through
       their registration. Public because the C callbacks in the implementation
       have to call them. */
    bool register_entry(std::uint32_t stage, std::uint32_t id, DispatchEntry entry);
    bool replace_tracked(std::uint32_t stage, std::uint32_t id, DispatchEntry entry);

    /* True when the assembly registered at least one entry for the stage; the
       host registers an engine system only for those. */
    bool usesStage(ResonateStage stage) const;

    /* The component whose chunks ride the context, one invocation per
       non-empty chunk (its name as registered, e.g.
       "resonate.hierarchy.LocalTransform"). Empty leaves the chunk fields
       null. */
    void set_component(const char* name);

    /* The UPDATE-stage run function, for hosts that schedule one stage only;
       `context` is this object. */
    static void invoke(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                       float delta_seconds);

    /* The per-stage run function the host registers through stageEntries. */
    static void invokeStage(ResonateStage stage, void* context, ecs::World& world,
                            ecs::CommandBuffer& commands, float delta_seconds);

    /* Fixed steps the UPDATE stage has run: the frame index a managed entry
       sees. Other stages observe the current step. */
    std::uint64_t frames() const
    {
        return frames_;
    }

private:
    RuntimeHost host_;
    std::array<DispatchTable, RESONATE_STAGE_COUNT> tables_{};
    bindings::SystemContext frame_{};
    std::uint64_t frames_ = 0U;
    void* registration_ = nullptr;
    std::string component_name_;
    ecs::ComponentIndex component_ = ecs::kInvalidComponent;
    ecs::Query query_;
    /* The world the cached query belongs to; a run that hands over another
       world rebuilds it instead of iterating memory it no longer owns. */
    const ecs::World* query_world_ = nullptr;
    bool query_ready_ = false;
    bindings::HostApi api_{};
    const gameplay::ActionMap* actions_ = nullptr;

    /* The stage's chunks for the single boundary call; reserved once so a
       frame does not allocate. */
    std::vector<bindings::ChunkSpan> chunk_spans_;

    struct PreviousEntry
    {
        std::uint32_t stage = 0U;
        std::uint32_t id = 0U;
        DispatchEntry entry = nullptr;
    };

    struct PatchFrame
    {
        void* context = nullptr;
        std::vector<PreviousEntry> previous;
    };
    void restoreFrame(PatchFrame& frame);
    std::vector<PatchFrame> patch_frames_;
    void* bootstrap_unload_ = nullptr;
};

} // namespace resonate::csharp

#endif /* RESONATE_CSHARP_MANAGED_SYSTEMS_H */
