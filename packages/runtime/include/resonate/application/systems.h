#ifndef RESONATE_APPLICATION_SYSTEMS_H
#define RESONATE_APPLICATION_SYSTEMS_H

#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/module/stage.h>

/*
 * The engine's own C++ systems: the same registration a plugin makes, through
 * the ABI's world-aware descriptor, with a run function that takes the world
 * and a command buffer. Without a world there is nothing to record into, so
 * the host refuses such a registration.
 */

namespace resonate
{

class ModuleHost;

struct EngineSystemDesc
{
    /* Borrowed: the schedule copies the name, and the run function and context
       are read every frame, so this descriptor must outlive the registration. */
    const char* name = nullptr;
    ResonateStage stage = RESONATE_STAGE_UPDATE;
    void (*run)(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                float delta_seconds) = nullptr;
    void* context = nullptr;
};

/* Structural changes go into the buffer this system is handed; the frame's sync
   points play it, which is what makes a spawn visible to the stage after the
   next one. Refused with RESONATE_E_STATE when the host has no world. */
ResonateStatus addEngineSystem(ModuleHost& host, const EngineSystemDesc& desc);

} // namespace resonate

#endif /* RESONATE_APPLICATION_SYSTEMS_H */
