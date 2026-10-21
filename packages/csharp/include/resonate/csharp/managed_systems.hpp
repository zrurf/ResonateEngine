#ifndef RESONATE_CSHARP_MANAGED_SYSTEMS_H
#define RESONATE_CSHARP_MANAGED_SYSTEMS_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <resonate/csharp/bindings.h>
#include <resonate/csharp/dispatch.h>
#include <resonate/csharp/host.hpp>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>

namespace resonate::csharp
{

/* The registration entry every managed assembly exposes; the binding generator
   will name it per assembly later. */
inline constexpr const char* kRegisterEntry = "Register";

/* A managed assembly's systems as one engine system. The assembly registers its
   entries into the table when it loads; the frame loop invokes them through the
   table with the generated frame context, so no delegate reaches the engine. */
class ManagedSystems
{
public:
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

    /* The registration callback a patch reaches through its registration; it
       remembers the entry being displaced. Public because the C callback in
       the implementation has to call it. */
    bool replace_tracked(std::uint32_t id, DispatchEntry entry);

    /* The component whose first non-empty chunk rides the context each frame
       (its name as registered, e.g. "resonate.hierarchy.LocalTransform").
       Empty leaves the chunk fields null. */
    void set_component(const char* name);

    /* The engine system's run function; `context` is this object. */
    static void invoke(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                       float delta_seconds);

    /* Frames the table has been invoked for. */
    std::uint64_t frames() const
    {
        return frames_;
    }

private:
    RuntimeHost host_;
    DispatchTable table_{64U};
    bindings::SystemContext frame_{};
    std::uint64_t frames_ = 0U;
    void* registration_ = nullptr;
    std::string component_name_;
    ecs::ComponentIndex component_ = ecs::kInvalidComponent;
    ecs::Query query_;
    bool query_ready_ = false;
    bindings::HostApi api_{};

    struct PatchFrame
    {
        void* context = nullptr;
        std::vector<std::pair<std::uint32_t, DispatchEntry>> previous;
    };
    void restoreFrame(PatchFrame& frame);
    std::vector<PatchFrame> patch_frames_;
    void* bootstrap_unload_ = nullptr;
};

} // namespace resonate::csharp

#endif /* RESONATE_CSHARP_MANAGED_SYSTEMS_H */
