#include <resonate/hierarchy/tree.h>

#include <cstddef>
#include <cstring>
#include <vector>

namespace resonate::hierarchy
{
namespace
{

using ecs::BlobHandle;
using ecs::Entity;
using ecs::World;

/* What setParent and clearParent record; destroyEntity records the same shape
   with the entity in `child`. */
struct TreeOp
{
    Entity child;
    Entity parent;
};

const char* const kHierarchy = "hierarchy";

/* --- the committed tree --- */

/* The depth of `from` in the committed tree, failing when the walk reaches
   `forbidden` (the move would create a cycle) or when it leaves kMaxDepth
   without finding a root. */
bool depthOf(const World& world, Entity from, Entity forbidden, std::uint32_t& depth)
{
    Entity current = from;
    depth = 0;
    for (;;)
    {
        if (current == forbidden)
        {
            return false;
        }

        const Parent* parent = world.get<Parent>(current);
        if (parent == nullptr || !world.alive(parent->parent))
        {
            return true;
        }
        if (depth == kMaxDepth)
        {
            return false;
        }
        current = parent->parent;
        ++depth;
    }
}

/* The height of the subtree rooted at `entity` (a leaf is 0), failing when a
   node would sit below the `budget` of levels left. */
bool subtreeHeight(const World& world, Entity entity, std::uint32_t budget, std::uint32_t& height)
{
    height = 0;

    const Children* kids = world.get<Children>(entity);
    if (kids == nullptr || !world.alive(kids->children))
    {
        return true;
    }

    const std::size_t bytes = world.blobSize(kids->children);
    const Entity* list = static_cast<const Entity*>(world.blobData(kids->children));
    for (std::size_t index = 0; index < bytes / sizeof(Entity); ++index)
    {
        if (!world.alive(list[index]))
        {
            continue;
        }
        if (budget == 0U)
        {
            return false;
        }
        std::uint32_t childHeight = 0;
        if (!subtreeHeight(world, list[index], budget - 1U, childHeight))
        {
            return false;
        }
        if (childHeight >= height)
        {
            height = childHeight + 1U;
        }
    }
    return true;
}

/* Appends `child` to `parent`'s list, creating the component and its blob the
   first time. */
void appendChild(World& world, Entity parent, Entity child)
{
    const Children* current = world.get<Children>(parent);
    BlobHandle handle = current != nullptr ? current->children : BlobHandle{};
    const bool hadComponent = current != nullptr;
    if (!world.alive(handle))
    {
        handle = world.createBlob(parent, 0);
        if (!handle.valid())
        {
            return;
        }
    }

    const std::size_t size = world.blobSize(handle);
    if (!world.resizeBlob(handle, size + sizeof(Entity)))
    {
        return;
    }
    std::memcpy(static_cast<std::byte*>(world.blobData(handle)) + size, &child, sizeof(Entity));

    if (!hadComponent)
    {
        const Children value{handle};
        if (world.add<Children>(parent, value) == nullptr)
        {
            world.report(World::Report::Error,
                         "%s: the Children component could not be added to entity %u:%u",
                         kHierarchy, parent.index, parent.generation);
            (void)world.destroyBlob(handle);
        }
    }
}

/* Removes the child from `parent`'s list, keeping the order of the rest. The
   list emptying takes the component — and its blob — away with it, which is
   the invariant the reads rely on. */
void removeChild(World& world, Entity parent, Entity child)
{
    const Children* current = world.get<Children>(parent);
    if (current == nullptr || !world.alive(current->children))
    {
        return;
    }

    const BlobHandle handle = current->children;
    const std::size_t count = world.blobSize(handle) / sizeof(Entity);
    Entity* list = static_cast<Entity*>(world.blobData(handle));
    for (std::size_t index = 0; index < count; ++index)
    {
        if (list[index] != child)
        {
            continue;
        }

        if (count == 1U)
        {
            (void)world.remove<Children>(parent);
            (void)world.destroyBlob(handle);
            return;
        }
        std::memmove(list + index, list + index + 1U, (count - index - 1U) * sizeof(Entity));
        (void)world.resizeBlob(handle, (count - 1U) * sizeof(Entity));
        return;
    }
}

/* Detaches `child` from whatever parent it currently has, if any. */
void detachFromParent(World& world, Entity child)
{
    const Parent* current = world.get<Parent>(child);
    if (current == nullptr)
    {
        return;
    }
    const Entity parent = current->parent;
    if (world.alive(parent))
    {
        removeChild(world, parent, child);
    }
    world.remove<Parent>(child);
}

/* --- the recorded commands, applied at playback --- */

void playSetParent(void*, World& world, const void* payload)
{
    const auto* op = static_cast<const TreeOp*>(payload);
    if (!world.alive(op->child) || !world.alive(op->parent))
    {
        world.report(World::Report::Error,
                     "%s: set-parent to %u:%u: an entity is not alive at playback", kHierarchy,
                     op->parent.index, op->parent.generation);
        return;
    }

    if (const Parent* current = world.get<Parent>(op->child))
    {
        if (current->parent == op->parent)
        {
            return; /* already a child of it */
        }
    }

    std::uint32_t parentDepth = 0;
    if (!depthOf(world, op->parent, op->child, parentDepth) || parentDepth + 1U > kMaxDepth)
    {
        world.report(World::Report::Error,
                     "%s: set-parent of %u:%u under %u:%u would create a cycle or push the tree "
                     "past %u levels",
                     kHierarchy, op->child.index, op->child.generation, op->parent.index,
                     op->parent.generation, kMaxDepth);
        return;
    }

    std::uint32_t height = 0;
    if (!subtreeHeight(world, op->child, kMaxDepth - (parentDepth + 1U), height))
    {
        world.report(World::Report::Error,
                     "%s: set-parent of %u:%u under %u:%u would push the subtree past %u levels",
                     kHierarchy, op->child.index, op->child.generation, op->parent.index,
                     op->parent.generation, kMaxDepth);
        return;
    }

    detachFromParent(world, op->child);

    const Parent value{op->parent};
    world.add<Parent>(op->child, value);
    appendChild(world, op->parent, op->child);
}

void playClearParent(void*, World& world, const void* payload)
{
    const auto* op = static_cast<const TreeOp*>(payload);
    if (!world.alive(op->child))
    {
        world.report(World::Report::Error, "%s: clearParent: entity %u:%u is not alive", kHierarchy,
                     op->child.index, op->child.generation);
        return;
    }
    detachFromParent(world, op->child);
}

void playDestroy(void*, World& world, const void* payload)
{
    const auto* op = static_cast<const TreeOp*>(payload);
    if (!world.alive(op->child))
    {
        return;
    }

    /* The children are read out first: detaching and destroying reclaims the
       blob the list lives in. They stay alive and become roots — a node's
       destruction does not delete a subtree silently. */
    const Children* kids = world.get<Children>(op->child);
    if (kids != nullptr && world.alive(kids->children))
    {
        const std::size_t count = world.blobSize(kids->children) / sizeof(Entity);
        std::vector<Entity> list(count);
        std::memcpy(list.data(), world.blobData(kids->children), count * sizeof(Entity));

        for (const Entity child : list)
        {
            if (!world.alive(child))
            {
                continue;
            }
            if (const Parent* parent = world.get<Parent>(child);
                parent != nullptr && parent->parent == op->child)
            {
                world.remove<Parent>(child);
            }
        }

        (void)world.remove<Children>(op->child);
        (void)world.destroyBlob(kids->children);
    }

    detachFromParent(world, op->child);
}

void recordTreeOp(ecs::CommandBuffer& commands, Entity child, Entity parent,
                  ecs::CommandBuffer::PlayFn play)
{
    const TreeOp op{child, parent};
    commands.record(play, nullptr, &op, sizeof(op));
}

} // namespace

bool registerComponents(ecs::World& world)
{
    /* Every call has to run: registration is idempotent, but it is also what
       reports a name collision. */
    bool ok = world.registerComponent<Parent>() != ecs::kInvalidComponent;
    ok = world.registerComponent<Children>() != ecs::kInvalidComponent && ok;
    ok = world.registerComponent<LocalTransform>() != ecs::kInvalidComponent && ok;
    ok = world.registerComponent<WorldTransform>() != ecs::kInvalidComponent && ok;
    return ok;
}

void setParent(ecs::CommandBuffer& commands, Entity child, Entity parent)
{
    if (!child.valid() || !parent.valid() || child == parent)
    {
        commands.world().report(ecs::World::Report::Error,
                                "%s: set-parent needs two different live handles", kHierarchy);
        return;
    }
    recordTreeOp(commands, child, parent, &playSetParent);
}

void clearParent(ecs::CommandBuffer& commands, Entity child)
{
    if (!child.valid())
    {
        commands.world().report(ecs::World::Report::Error, "%s: clearParent needs a live handle",
                                kHierarchy);
        return;
    }
    recordTreeOp(commands, child, Entity{}, &playClearParent);
}

void destroyEntity(ecs::CommandBuffer& commands, Entity entity)
{
    if (!entity.valid())
    {
        commands.world().report(ecs::World::Report::Error, "%s: destroyEntity needs a live handle",
                                kHierarchy);
        return;
    }

    /* The op runs first, so the lists are consistent before the entity dies. */
    recordTreeOp(commands, entity, Entity{}, &playDestroy);
    commands.destroy(entity);
}

Entity parentOf(const ecs::World& world, Entity entity) noexcept
{
    if (!world.alive(entity))
    {
        return Entity{};
    }
    const Parent* parent = world.get<Parent>(entity);
    return parent != nullptr ? parent->parent : Entity{};
}

Span<const ecs::Entity> children(const ecs::World& world, Entity entity) noexcept
{
    if (!world.alive(entity))
    {
        return Span<const ecs::Entity>{};
    }
    const Children* kids = world.get<Children>(entity);
    if (kids == nullptr || !world.alive(kids->children))
    {
        return Span<const ecs::Entity>{};
    }

    const std::size_t bytes = world.blobSize(kids->children);
    return Span<const ecs::Entity>(static_cast<const Entity*>(world.blobData(kids->children)),
                                   bytes / sizeof(Entity));
}

std::uint32_t childCount(const ecs::World& world, Entity entity) noexcept
{
    return static_cast<std::uint32_t>(children(world, entity).size());
}

bool isAncestorOf(const ecs::World& world, Entity ancestor, Entity entity) noexcept
{
    if (!world.alive(ancestor) || !world.alive(entity))
    {
        return false;
    }

    for (std::uint32_t step = 0; step <= kMaxDepth; ++step)
    {
        if (entity == ancestor)
        {
            return true;
        }
        const Entity parent = parentOf(world, entity);
        if (!parent.valid())
        {
            return false;
        }
        entity = parent;
    }
    return false;
}

} // namespace resonate::hierarchy
