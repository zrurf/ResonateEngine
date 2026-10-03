#ifndef RESONATE_HIERARCHY_TREE_H
#define RESONATE_HIERARCHY_TREE_H

#include <cstdint>

#include <resonate/core/span.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

#include "resonate.hierarchy/components.gen.h"

/*
 * The scene tree: entities linked by the Parent/Children components.
 *
 * Edits are structural commands (law 2): they are recorded into a command
 * buffer and applied when the frame's sync points play it. Playback is also
 * where the invariants are enforced — no cycles, no node deeper than
 * kMaxDepth, every parent's child list agreeing with its children — because
 * only playback sees the state the earlier records of the same buffer
 * produced.
 */
namespace resonate::hierarchy
{

/* The deepest a node may sit: the bound the propagation walks by, and what
   keeps a corrupted tree from turning a walk into an endless one. */
inline constexpr std::uint32_t kMaxDepth = 64;

/* Registers the tree's four components in a world. Call it once per world
   before anything else here; registering again is harmless, and a refusal
   (a name collision or a full type table) is reported and answers false. */
bool registerComponents(ecs::World& world);

/* Records making `child` a child of `parent`, moving it out of any current
   parent. A command the playback refuses — with a report, applying nothing —
   when either entity is not alive at that point, when the move would fold the
   tree into a cycle, or when it would push the moved subtree past kMaxDepth. */
void setParent(ecs::CommandBuffer& commands, ecs::Entity child, ecs::Entity parent);

/* Records detaching `child` from its parent. A child without a parent is a
   no-op. */
void clearParent(ecs::CommandBuffer& commands, ecs::Entity child);

/* Records detaching and destroying an entity: its children are detached (they
   become roots), its parent forgets it, and its blobs are reclaimed with it.
   Destroying a tree node through `commands.destroy` alone does not do this —
   it leaves a dead handle in the parent's child list. */
void destroyEntity(ecs::CommandBuffer& commands, ecs::Entity entity);

/* --- reading the committed tree --- */

/* The stored parent handle, or an invalid one when the entity has no parent,
   is not alive, or never had the component. A parent destroyed outside the
   tree commands is returned as stored; `alive` is the caller's check. */
[[nodiscard]] ecs::Entity parentOf(const ecs::World& world, ecs::Entity entity) noexcept;

/* The children in insertion order. The span borrows the Children blob, which
   lives and dies like a chunk view: valid until the next structural change. */
[[nodiscard]] Span<const ecs::Entity> children(const ecs::World& world,
                                               ecs::Entity entity) noexcept;

[[nodiscard]] std::uint32_t childCount(const ecs::World& world, ecs::Entity entity) noexcept;

/* Whether `ancestor` is `entity` itself or above it. The walk is bounded by
   kMaxDepth, so a corrupted tree answers false instead of looping. */
[[nodiscard]] bool isAncestorOf(const ecs::World& world, ecs::Entity ancestor,
                                ecs::Entity entity) noexcept;

} // namespace resonate::hierarchy

#endif /* RESONATE_HIERARCHY_TREE_H */
