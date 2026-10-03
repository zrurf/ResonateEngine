#include <resonate/application/systems.h>

#include <resonate/module/host.hpp>

namespace resonate
{
namespace
{

/* The ABI's run form over the typed one: the handles are the host's pointers. */
void runTyped(void* context, ResonateWorld* world, ResonateCommands* commands, float delta_seconds)
{
    const auto* desc = static_cast<const EngineSystemDesc*>(context);
    desc->run(desc->context, *static_cast<ecs::World*>(world->instance),
              *static_cast<ecs::CommandBuffer*>(commands->instance), delta_seconds);
}

} // namespace

ResonateStatus addEngineSystem(ModuleHost& host, const EngineSystemDesc& desc)
{
    if (desc.name == nullptr || desc.run == nullptr)
    {
        return RESONATE_E_INVALID;
    }

    ResonateSystemDesc abi = {};
    abi.struct_size = sizeof(ResonateSystemDesc);
    abi.stage = desc.stage;
    abi.name = desc.name;
    abi.run_world = &runTyped;
    abi.context = const_cast<EngineSystemDesc*>(&desc);
    return host.addSystem(abi);
}

} // namespace resonate
