#include <resonate/ecs/query.h>

#include <cstdint>
#include <vector>

#include "world_internal.h"

namespace resonate::ecs
{
namespace
{

using detail::Archetype;
using detail::Chunk;

bool matches(const Archetype& archetype, std::uint64_t all, std::uint64_t any,
             std::uint64_t none) noexcept
{
    if ((archetype.mask & all) != all)
    {
        return false;
    }
    if (any != 0U && (archetype.mask & any) == 0U)
    {
        return false;
    }
    return (archetype.mask & none) == 0U;
}

/* The dense view of one chunk, over the caller's column buffer. */
ChunkView makeView(Chunk* chunk, ChunkView::Column* columns) noexcept
{
    const Archetype& archetype = *chunk->archetype;
    for (std::uint32_t slot = 0; slot < archetype.componentCount; ++slot)
    {
        const ComponentIndex component = archetype.components[slot];
        columns[slot].component = component;
        columns[slot].data = detail::componentData(chunk, component);
        columns[slot].ticks = detail::componentTicks(chunk, component);
    }

    ChunkView view;
    view.count = chunk->count;
    view.entities = detail::entitiesOf(chunk);
    view.columns = columns;
    view.columnCount = archetype.componentCount;
    view.chunk = chunk;
    view.version = chunk->version;
    return view;
}

} // namespace

bool ChunkView::valid() const noexcept
{
    return chunk != nullptr && static_cast<const detail::Chunk*>(chunk)->version == version;
}

void* ChunkView::componentData(ComponentIndex component) const noexcept
{
    for (std::uint32_t index = 0; index < columnCount; ++index)
    {
        if (columns[index].component == component)
        {
            return columns[index].data;
        }
    }
    return nullptr;
}

std::uint32_t* ChunkView::componentTicks(ComponentIndex component) const noexcept
{
    for (std::uint32_t index = 0; index < columnCount; ++index)
    {
        if (columns[index].component == component)
        {
            return columns[index].ticks;
        }
    }
    return nullptr;
}

Query::Query(World& world, const QueryDesc& desc) : world_(&world), impl_(world.impl_.get())
{
    valid_ = impl_ != nullptr;

    const Span<const ComponentIndex> lists[3] = {desc.all, desc.any, desc.none};
    std::uint64_t* masks[3] = {&allMask_, &anyMask_, &noneMask_};
    for (std::size_t list = 0; list < 3; ++list)
    {
        for (const ComponentIndex component : lists[list])
        {
            if (impl_ != nullptr && !impl_->requireType(component, "query"))
            {
                valid_ = false;
                continue;
            }
            *masks[list] |= detail::bitOf(component);
        }
    }

    if (valid_ && (allMask_ & noneMask_) != 0U)
    {
        impl_->say(World::Report::Warning,
                   "query asks for a component it also excludes; it can never match");
    }
}

void Query::refresh()
{
    if (impl_ == nullptr)
    {
        return;
    }
    const std::vector<std::unique_ptr<Archetype>>& archetypes = impl_->archetypes;
    for (; examined_ < archetypes.size(); ++examined_)
    {
        Archetype& archetype = *archetypes[examined_];
        if (matches(archetype, allMask_, anyMask_, noneMask_))
        {
            matched_.push_back(&archetype);
        }
    }
}

void Query::collect(std::vector<Chunk*>& chunks)
{
    refresh();
    chunks.clear();
    for (Archetype* archetype : matched_)
    {
        for (Chunk* chunk : archetype->chunks)
        {
            if (chunk->count != 0U)
            {
                chunks.push_back(chunk);
            }
        }
    }
}

std::uint32_t Query::matchedArchetypeCount()
{
    refresh();
    return static_cast<std::uint32_t>(matched_.size());
}

std::uint32_t Query::matchedChunkCount()
{
    collect(chunks_);
    return static_cast<std::uint32_t>(chunks_.size());
}

void Query::forEachChunk(ChunkFunction body, void* context)
{
    if (!valid_ || body == nullptr)
    {
        return;
    }

    refresh();

    /* Iteration hands out pointers into chunks; a structural change would move
       the rows under them, so the guard is up for the whole visit. */
    const World::ParallelScope parallel(*world_);
    ChunkView::Column columns[kMaxComponentTypes];

    for (Archetype* archetype : matched_)
    {
        for (Chunk* chunk : archetype->chunks)
        {
            if (chunk->count != 0U)
            {
                body(context, makeView(chunk, columns));
            }
        }
    }
}

void Query::parallelEachChunk(JobSystem& jobs, JobGroup reads, JobGroup writes, ChunkFunction body,
                              void* context)
{
    if (!valid_ || body == nullptr)
    {
        return;
    }

    collect(chunks_);
    if (chunks_.empty())
    {
        return;
    }

    struct State
    {
        const std::vector<Chunk*>* chunks;
        ChunkFunction body;
        void* context;
    };
    State state{&chunks_, body, context};

    const World::ParallelScope parallel(*world_);
    const JobIndex handle = jobs.submitParallel(
        [](void* job_context, std::uint32_t begin, std::uint32_t end)
        {
            auto* state = static_cast<State*>(job_context);
            ChunkView::Column columns[kMaxComponentTypes];
            for (std::uint32_t index = begin; index < end; ++index)
            {
                state->body(state->context, makeView((*state->chunks)[index], columns));
            }
        },
        &state, static_cast<std::uint32_t>(chunks_.size()), reads, writes, JobPriorityHigh);
    jobs.wait(handle);
}

} // namespace resonate::ecs
