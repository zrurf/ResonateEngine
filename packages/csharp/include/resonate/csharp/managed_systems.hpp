#ifndef RESONATE_CSHARP_MANAGED_SYSTEMS_H
#define RESONATE_CSHARP_MANAGED_SYSTEMS_H

#include <cstdint>
#include <string>

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
};

} // namespace resonate::csharp

#endif /* RESONATE_CSHARP_MANAGED_SYSTEMS_H */
