#ifndef RESONATE_HIERARCHY_NAME_H
#define RESONATE_HIERARCHY_NAME_H

#include <string>
#include <string_view>

#include <resonate/ecs/world.h>

namespace resonate::hierarchy
{

/* The entity's display name: UTF-8 bytes in the blob store, carried by the
   Name component. An entity without the component, or one whose blob is
   empty, has no name. */
[[nodiscard]] std::string nameOf(const ecs::World& world, ecs::Entity entity);

/* Replaces the entity's name: the blob is rewritten and the component marked.
   An empty name clears the bytes. False with `error` set when the entity is
   dead or the blob store refuses. */
bool setName(ecs::World& world, ecs::Entity entity, std::string_view utf8,
             std::string& error);

} // namespace resonate::hierarchy

#endif /* RESONATE_HIERARCHY_NAME_H */
