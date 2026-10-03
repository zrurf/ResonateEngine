#ifndef RESONATE_ECS_ENTITY_H
#define RESONATE_ECS_ENTITY_H

#include <cstdint>

namespace resonate::ecs
{

/* Generation-tagged handle: the generation distinguishes a recycled slot from
   the entity that used to occupy it. A handle is the only reference that may be
   held across a frame or a structural change; a pointer into a chunk lives only
   as long as nothing moves. */
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

} // namespace resonate::ecs

#endif /* RESONATE_ECS_ENTITY_H */
