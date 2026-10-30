#ifndef RESONATE_SCENE_CAPTURE_H
#define RESONATE_SCENE_CAPTURE_H

/*
 * The scene writer: the subtree under a root object written back as a .rscene
 * document. Keys come from the SourceKey each instantiated object carries,
 * names from Name, and the scene schema's composed components are written with
 * only the fields that differ from their schema defaults, so a capture reads
 * like the document it came from. Procedural entities outside the subtree
 * (the demo's light grid) are not written. A resolved texture reference is
 * written back as its asset id, never as the slot it happened to occupy.
 */

#include <string>

#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::scene
{

/* The topmost ancestor of `entity`: the scene root a capture walks. An entity
   with no Parent answers itself. */
[[nodiscard]] ecs::Entity rootOf(ecs::World& world, ecs::Entity entity);

/* Writes the subtree rooted at `root` as a scene document. False with a named
   error when the root is not alive. */
[[nodiscard]] bool capture(ecs::World& world, ecs::Entity root, std::string& out_xml,
                           std::string& error);

} // namespace resonate::scene

#endif /* RESONATE_SCENE_CAPTURE_H */
