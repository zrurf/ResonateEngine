#include <resonate/hierarchy/system.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <resonate/core/math.h>
#include <resonate/ecs/query.h>
#include <resonate/hierarchy/tree.h>

#include "resonate.hierarchy/components.gen.h"

namespace resonate::hierarchy
{
namespace
{

using resonate::JobIndex;
using resonate::ecs::ChunkView;
using resonate::ecs::ComponentIndex;
using resonate::ecs::Entity;
using resonate::ecs::World;

/* No participant sits here yet. */
constexpr std::uint32_t kUnvisited = 0xFFFFFFFFU;

/* One worker's share of one level: the nodes of that level, all of whose
   parents were written by an earlier one. */
struct LevelSlice
{
    const Entity* nodes = nullptr;
    World* world = nullptr;
};

void computeLevel(void* context, std::uint32_t begin, std::uint32_t end)
{
    auto* slice = static_cast<LevelSlice*>(context);
    World& world = *slice->world;

    for (std::uint32_t index = begin; index < end; ++index)
    {
        const Entity entity = slice->nodes[index];
        const LocalTransform* local = world.get<LocalTransform>(entity);
        WorldTransform* worldTransform = world.get<WorldTransform>(entity);
        if (local == nullptr || worldTransform == nullptr)
        {
            continue;
        }

        const Mat4 localMatrix = composeTrs(local->position, local->rotation, local->scale);
        const Parent* parent = world.get<Parent>(entity);
        if (parent != nullptr && world.alive(parent->parent))
        {
            if (const WorldTransform* parentWorld = world.get<WorldTransform>(parent->parent))
            {
                worldTransform->matrix = parentWorld->matrix * localMatrix;
                continue;
            }
        }

        /* A root, or a parent that is not a transform node: the local matrix is
           the whole story. */
        worldTransform->matrix = localMatrix;
    }
}

} // namespace

struct PropagationSystem::Impl
{
    resonate::ecs::Query query;
    World* bound = nullptr;

    /* Per run: every transform node, its local matrix, the node slot an entity
       index maps to, the level each slot was placed at, and the level arrays
       themselves. */
    std::vector<Entity> nodes;
    std::vector<Mat4> locals;
    std::vector<std::uint32_t> slotOf;
    std::vector<std::uint32_t> levelOf;
    std::vector<Entity> levels[kMaxDepth + 1];

    std::uint32_t staleChildren = 0;
    std::uint32_t unreachable = 0;
    std::uint32_t overDeep = 0;

    struct Collect
    {
        Impl* impl = nullptr;
    };

    static void collect(void* context, ChunkView view);

    /* The slot of a handle that is one of this run's participants. */
    [[nodiscard]] std::uint32_t participantSlot(Entity entity) const noexcept
    {
        if (entity.index >= slotOf.size())
        {
            return kUnvisited;
        }
        const std::uint32_t slot = slotOf[entity.index];
        if (slot == kUnvisited || nodes[slot] != entity)
        {
            return kUnvisited;
        }
        return slot;
    }

    void run(World& world, JobSystem* jobs);
};

void PropagationSystem::Impl::collect(void* context, ChunkView view)
{
    auto* state = static_cast<Collect*>(context);
    Impl& impl = *state->impl;

    const resonate::ecs::Column<LocalTransform> local = resonate::ecs::column<LocalTransform>(view);
    for (std::uint32_t row = 0; row < view.count; ++row)
    {
        const Entity entity = view.entities[row];
        const std::uint32_t slot = static_cast<std::uint32_t>(impl.nodes.size());
        impl.nodes.push_back(entity);
        impl.locals.push_back(
            composeTrs(local.data[row].position, local.data[row].rotation, local.data[row].scale));

        if (entity.index >= impl.slotOf.size())
        {
            impl.slotOf.resize(entity.index + 1U, kUnvisited);
        }
        impl.slotOf[entity.index] = slot;
    }
}

void PropagationSystem::Impl::run(World& world, JobSystem* jobs)
{
    if (bound != &world)
    {
        const ComponentIndex all[] = {ecs::ComponentTraits<LocalTransform>::index,
                                      ecs::ComponentTraits<WorldTransform>::index};
        query = world.createQuery(ecs::QueryDesc{all, {}, {}});
        bound = &world;
    }
    if (!query.valid())
    {
        return; /* refused at creation, and reported there */
    }

    nodes.clear();
    locals.clear();
    std::fill(slotOf.begin(), slotOf.end(), kUnvisited);
    staleChildren = 0;
    unreachable = 0;
    overDeep = 0;
    for (std::vector<Entity>& level : levels)
    {
        level.clear();
    }

    Collect state{this};
    query.forEachChunk(&PropagationSystem::Impl::collect, &state);
    if (nodes.empty())
    {
        return;
    }
    levelOf.assign(nodes.size(), kUnvisited);

    /* The roots: nodes whose parent is not one of the participants. */
    for (std::uint32_t slot = 0; slot < nodes.size(); ++slot)
    {
        const Parent* parent = world.get<Parent>(nodes[slot]);
        if (parent != nullptr && world.alive(parent->parent) &&
            participantSlot(parent->parent) != kUnvisited)
        {
            continue;
        }
        levelOf[slot] = 0;
        levels[0].push_back(nodes[slot]);
    }

    for (std::uint32_t depth = 0; depth <= kMaxDepth; ++depth)
    {
        const std::vector<Entity>& level = levels[depth];
        if (!level.empty())
        {
            LevelSlice slice{level.data(), &world};
            const auto count = static_cast<std::uint32_t>(level.size());
            if (jobs == nullptr)
            {
                computeLevel(&slice, 0, count);
            }
            else
            {
                const resonate::ecs::World::ParallelScope parallel(world);
                const JobIndex handle =
                    jobs->submitParallel(&computeLevel, &slice, count, 0, 0, JobPriorityHigh);
                jobs->wait(handle);
            }
        }

        /* Discovery: the participants among this level's children land one
           level down. Children that are not transform nodes are passed over —
           their own transform children were classified as roots. */
        for (const Entity entity : level)
        {
            for (const Entity child : children(world, entity))
            {
                if (!world.alive(child))
                {
                    ++staleChildren;
                    continue;
                }
                const std::uint32_t slot = participantSlot(child);
                if (slot == kUnvisited || levelOf[slot] != kUnvisited)
                {
                    continue;
                }
                if (depth == kMaxDepth)
                {
                    ++overDeep;
                    continue;
                }
                levelOf[slot] = depth + 1U;
                levels[depth + 1U].push_back(child);
            }
        }
    }

    for (const std::uint32_t placed : levelOf)
    {
        if (placed == kUnvisited)
        {
            ++unreachable;
        }
    }

    if (unreachable != 0)
    {
        world.report(World::Report::Warning,
                     "hierarchy: %u transform node(s) are unreachable from any root", unreachable);
    }
    if (staleChildren != 0)
    {
        world.report(World::Report::Warning,
                     "hierarchy: %u stale child handle(s) in children lists", staleChildren);
    }
    if (overDeep != 0)
    {
        world.report(World::Report::Warning, "hierarchy: %u node(s) sit below the %u level limit",
                     overDeep, kMaxDepth);
    }
}

PropagationSystem::PropagationSystem() : impl_(std::make_unique<Impl>())
{
}

PropagationSystem::~PropagationSystem() = default;

void PropagationSystem::run(ecs::World& world)
{
    impl_->run(world, jobs);
}

void PropagationSystem::invoke(void* context, ecs::World& world, ecs::CommandBuffer&, float)
{
    static_cast<PropagationSystem*>(context)->run(world);
}

} // namespace resonate::hierarchy
