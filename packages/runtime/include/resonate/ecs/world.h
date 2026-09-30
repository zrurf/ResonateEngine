#ifndef RESONATE_ECS_WORLD_H
#define RESONATE_ECS_WORLD_H

#include <cstdint>

#include <resonate/module/stage.h>

/* Host-internal: modules reach these through a capability, never by linking
   against them. */

namespace resonate::ecs
{

/* Generation-tagged handle: the generation distinguishes a recycled slot from
   the entity that used to occupy it. */
struct Entity
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return generation != 0;
    }

    friend constexpr bool operator==(const Entity& a, const Entity& b) noexcept
    {
        return a.index == b.index && a.generation == b.generation;
    }
};

class World
{
  public:
    virtual ~World() = default;

    virtual Entity create() = 0;
    virtual void destroy(Entity entity) = 0;

    [[nodiscard]] virtual bool alive(Entity entity) const = 0;

    /* Systems run in enum order; see resonate/module/stage.h for the schedule. */
    virtual void runStage(ResonateStage stage, float delta_seconds) = 0;
};

/* A unit of per-frame work owned by the host. Plugins register work through
   ResonateSystemDesc; this class cannot cross the module boundary. */
class System
{
  public:
    virtual ~System() = default;

    virtual void onUpdate(World& world, float delta_seconds) = 0;

    [[nodiscard]] virtual ResonateStage stage() const = 0;
};

} // namespace resonate::ecs

#endif /* RESONATE_ECS_WORLD_H */
