#ifndef RESONATE_ECS_SRC_WORLD_INTERNAL_H
#define RESONATE_ECS_SRC_WORLD_INTERNAL_H

/* Private to the package: the storage's shape, shared by the world, the
   queries and the command buffer. */

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "resonate/ecs/world.h"

namespace resonate::ecs::detail
{

/* A chunk is one allocation of about this much; the entity capacity that fits
   falls out of the archetype's layout. */
inline constexpr std::uint32_t kChunkBytes = 16U * 1024U;
inline constexpr std::uint32_t kChunkAlignment = 64U;

/* Combination explosion shows up as archetype count long before it shows up as
   memory; this is the design's warning threshold. */
inline constexpr std::size_t kArchetypeWarnCount = 200U;

using ArchetypeIndex = std::uint16_t;
inline constexpr ArchetypeIndex kInvalidArchetype = 0xFFFF;

inline constexpr std::uint64_t bitOf(ComponentIndex component) noexcept
{
    return std::uint64_t{1} << component;
}

inline std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) noexcept
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

inline bool isPowerOfTwo(std::uint32_t value) noexcept
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

struct Chunk;
struct Archetype;

/* One slot per entity index; `chunk` is null while the index is free. A create
   recorded in a command buffer reserves the slot: the handle is final, the
   entity is not alive until playback materializes it. */
struct EntityRecord
{
    std::uint32_t generation = 0;
    std::uint32_t row = 0;
    Chunk* chunk = nullptr;
    bool reserved = false;
};

/* What a chunk carries after its header, matched to an archetype: entity
   handles, a chunk-level tick per component type, then per component the dense
   data array and the per-entity tick array. */
struct ChunkLayout
{
    std::uint32_t capacity = 0;
    std::uint32_t entitiesOffset = 0;
    std::uint32_t chunkTicksOffset = 0;
    std::uint32_t dataOffsets[kMaxComponentTypes] = {};
    std::uint32_t tickOffsets[kMaxComponentTypes] = {};

    /* Position of each component in the chunk-level tick array, which is dense
       over the archetype's components rather than over all component types. */
    std::uint16_t tickSlots[kMaxComponentTypes] = {};
};

struct Chunk
{
    Archetype* archetype = nullptr;
    std::uint32_t capacity = 0;
    std::uint32_t count = 0;

    /* Bumped on every structural change to this chunk. A view made before one
       is stale; ChunkView::valid() reports it (law 6). */
    std::uint32_t version = 0;
};

struct Archetype
{
    Archetype()
    {
        std::fill(std::begin(addEdge), std::end(addEdge), kInvalidArchetype);
        std::fill(std::begin(removeEdge), std::end(removeEdge), kInvalidArchetype);
    }

    ArchetypeIndex index = kInvalidArchetype;
    std::uint64_t mask = 0;
    std::uint32_t componentCount = 0;
    std::uint32_t entityCount = 0;

    /* Dense component indices in mask order, ascending. */
    std::uint16_t components[kMaxComponentTypes] = {};
    ChunkLayout layout;

    /* Migration edges, filled the first time a component is added or removed;
       crossing an edge is the archetype graph. */
    ArchetypeIndex addEdge[kMaxComponentTypes] = {};
    ArchetypeIndex removeEdge[kMaxComponentTypes] = {};

    std::vector<Chunk*> chunks;
};

/* The process-wide component universe: a schema-owned type has one identity, so
   the index a type is assigned is the same in every world. Names are borrowed
   from the traits and must be literals or generated constants. */
struct ComponentType
{
    const char* name = nullptr;
    std::uint32_t size = 0;
    std::uint32_t alignment = 0;
};

struct ComponentUniverse
{
    std::mutex mutex;
    ComponentType types[kMaxComponentTypes] = {};
    std::uint32_t count = 0;
};

inline ComponentUniverse& componentUniverse()
{
    static ComponentUniverse universe;
    return universe;
}

inline Entity* entitiesOf(Chunk* chunk) noexcept
{
    return reinterpret_cast<Entity*>(reinterpret_cast<std::byte*>(chunk) +
                                     chunk->archetype->layout.entitiesOffset);
}

inline std::uint32_t* chunkTicksOf(Chunk* chunk) noexcept
{
    return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(chunk) +
                                            chunk->archetype->layout.chunkTicksOffset);
}

inline std::byte* componentData(Chunk* chunk, ComponentIndex component) noexcept
{
    return reinterpret_cast<std::byte*>(chunk) + chunk->archetype->layout.dataOffsets[component];
}

inline std::uint32_t* componentTicks(Chunk* chunk, ComponentIndex component) noexcept
{
    return reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(chunk) +
                                            chunk->archetype->layout.tickOffsets[component]);
}

/* The entity capacity that fits kChunkBytes, every array aligned for its
   component. The guess starts from the unpadded per-entity size and shrinks
   until the padded layout fits, so capacity and offsets always agree. */
inline bool computeLayout(const std::uint32_t* sizes, const std::uint32_t* alignments,
                          const std::uint16_t* maskComponents, std::uint32_t componentCount,
                          ChunkLayout& layout)
{
    std::uint32_t perEntity = sizeof(Entity) + 4U * componentCount;
    for (std::uint32_t slot = 0; slot < componentCount; ++slot)
    {
        perEntity += sizes[maskComponents[slot]];
    }

    std::uint32_t capacity = (kChunkBytes - sizeof(Chunk)) / perEntity + 2U;
    for (; capacity > 0; --capacity)
    {
        std::uint32_t cursor = alignUp(static_cast<std::uint32_t>(sizeof(Chunk)),
                                       static_cast<std::uint32_t>(alignof(Entity)));
        layout.entitiesOffset = cursor;
        cursor += static_cast<std::uint32_t>(sizeof(Entity)) * capacity;

        cursor = alignUp(cursor, 4U);
        layout.chunkTicksOffset = cursor;
        cursor += 4U * componentCount;

        for (std::uint32_t slot = 0; slot < componentCount; ++slot)
        {
            const ComponentIndex component = maskComponents[slot];
            const std::uint32_t alignment = alignments[component] < 4U ? 4U : alignments[component];

            cursor = alignUp(cursor, alignment);
            layout.dataOffsets[component] = cursor;
            cursor += sizes[component] * capacity;

            cursor = alignUp(cursor, 4U);
            layout.tickOffsets[component] = cursor;
            cursor += 4U * capacity;

            layout.tickSlots[component] = static_cast<std::uint16_t>(slot);
        }

        if (cursor <= kChunkBytes)
        {
            layout.capacity = capacity;
            return true;
        }
    }
    return false;
}

struct WorldImpl
{
    struct ComponentInfo
    {
        const char* name = nullptr;
        std::uint32_t size = 0;
        std::uint32_t alignment = 0;
    };

    explicit WorldImpl(Allocator& allocator_in) : allocator(&allocator_in)
    {
    }

    Allocator* allocator = nullptr;
    void* report_user = nullptr;
    World::ReportFn report = nullptr;

    ComponentInfo components[kMaxComponentTypes] = {};

    /* Which process-wide component indices this world registered; a world only
       holds the types it asked for. */
    std::uint64_t registeredMask = 0;
    std::uint32_t registeredCount = 0;

    std::vector<EntityRecord> slots;
    std::vector<std::uint32_t> freeSlots;
    std::uint32_t alive = 0;

    std::vector<std::unique_ptr<Archetype>> archetypes;
    std::vector<Chunk*> pooled;

    std::atomic<ChangeTick> typeTicks[kMaxComponentTypes] = {};
    std::atomic<std::uint32_t> parallelDepth{0};

    /* --- reporting: error paths only, so the cost is the message itself --- */

    void say(World::Report level, const char* format, ...)
    {
        char message[256] = {};
        std::va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);

        if (report != nullptr)
        {
            report(report_user, level, message);
            return;
        }
        std::fprintf(stderr, "resonate: %s\n", message);
    }

    void reportStale(Entity entity, const char* what)
    {
        say(World::Report::Error, "%s: entity %u:%u is not alive", what, entity.index,
            entity.generation);
    }

    bool structural()
    {
        if (parallelDepth.load(std::memory_order_relaxed) == 0U)
        {
            return true;
        }
        say(World::Report::Error,
            "structural change refused: parallel execution is in flight (law 2)");
        return false;
    }

    /* Null for a stale, dead or out-of-range handle. */
    EntityRecord* recordOf(Entity entity, const char* what)
    {
        if (entity.index >= slots.size())
        {
            reportStale(entity, what);
            return nullptr;
        }
        EntityRecord& record = slots[entity.index];
        if (record.chunk == nullptr || record.generation != entity.generation)
        {
            reportStale(entity, what);
            return nullptr;
        }
        return &record;
    }

    bool registered(ComponentIndex component) const noexcept
    {
        return (registeredMask & bitOf(component)) != 0U;
    }

    bool hasType(ComponentIndex component) const noexcept
    {
        return component != kInvalidComponent && registered(component);
    }

    /* True when this world holds the type; otherwise reports, naming which of
       the two ways it does not. */
    bool requireType(ComponentIndex component, const char* what)
    {
        if (hasType(component))
        {
            return true;
        }
        if (component == kInvalidComponent)
        {
            say(World::Report::Error, "%s: the component type was never registered", what);
        }
        else
        {
            say(World::Report::Error, "%s: component %u is not registered in this world", what,
                component);
        }
        return false;
    }

    void registerType(ComponentIndex component, const char* name, std::uint32_t size,
                      std::uint32_t alignment)
    {
        if (registered(component))
        {
            return;
        }
        components[component] = ComponentInfo{name, size, alignment};
        registeredMask |= bitOf(component);
        ++registeredCount;
    }

    ComponentIndex findComponent(const char* name) const noexcept
    {
        if (name == nullptr)
        {
            return kInvalidComponent;
        }
        ComponentUniverse& universe = componentUniverse();
        const std::lock_guard<std::mutex> lock(universe.mutex);
        for (std::uint32_t candidate = 0; candidate < universe.count; ++candidate)
        {
            if (std::strcmp(universe.types[candidate].name, name) == 0)
            {
                const ComponentIndex index = static_cast<ComponentIndex>(candidate);
                return registered(index) ? index : kInvalidComponent;
            }
        }
        return kInvalidComponent;
    }

    /* --- archetypes --- */

    Archetype* archetypeForMask(std::uint64_t mask)
    {
        for (const std::unique_ptr<Archetype>& candidate : archetypes)
        {
            if (candidate->mask == mask)
            {
                return candidate.get();
            }
        }

        if ((mask & registeredMask) != mask)
        {
            say(World::Report::Error,
                "archetype mask 0x%llx names a component this world did not register",
                static_cast<unsigned long long>(mask));
            return nullptr;
        }

        std::uint16_t maskComponents[kMaxComponentTypes] = {};
        std::uint32_t count = 0;
        for (ComponentIndex component = 0; component < kMaxComponentTypes; ++component)
        {
            if ((mask & bitOf(component)) != 0U)
            {
                maskComponents[count++] = component;
            }
        }

        std::uint32_t sizes[kMaxComponentTypes] = {};
        std::uint32_t alignments[kMaxComponentTypes] = {};
        for (ComponentIndex index = 0; index < kMaxComponentTypes; ++index)
        {
            if (registered(index))
            {
                sizes[index] = components[index].size;
                alignments[index] = components[index].alignment;
            }
        }

        auto archetype = std::make_unique<Archetype>();
        if (!computeLayout(sizes, alignments, maskComponents, count, archetype->layout))
        {
            say(World::Report::Error,
                "component set 0x%llx leaves no room for one entity in a %u byte chunk",
                static_cast<unsigned long long>(mask), kChunkBytes);
            return nullptr;
        }

        archetype->mask = mask;
        archetype->componentCount = count;
        std::memcpy(archetype->components, maskComponents, sizeof(maskComponents));
        archetype->index = static_cast<ArchetypeIndex>(archetypes.size());

        Archetype* raw = archetype.get();
        archetypes.push_back(std::move(archetype));

        if (archetypes.size() == kArchetypeWarnCount + 1U)
        {
            say(World::Report::Warning,
                "more than %zu archetypes: component combinations are exploding",
                kArchetypeWarnCount);
        }
        return raw;
    }

    Archetype* emptyArchetype()
    {
        return archetypeForMask(0);
    }

    Archetype* edge(Archetype& from, ComponentIndex component, bool adding)
    {
        ArchetypeIndex& cached = adding ? from.addEdge[component] : from.removeEdge[component];
        if (cached != kInvalidArchetype)
        {
            return archetypes[cached].get();
        }

        const std::uint64_t mask =
            adding ? (from.mask | bitOf(component)) : (from.mask & ~bitOf(component));
        Archetype* target = archetypeForMask(mask);
        if (target == nullptr)
        {
            return nullptr;
        }
        cached = target->index;
        return target;
    }

    /* --- chunks --- */

    Chunk* takeChunk(Archetype& archetype)
    {
        Chunk* chunk = nullptr;
        if (!pooled.empty())
        {
            chunk = pooled.back();
            pooled.pop_back();
        }
        else
        {
            void* memory = allocator->allocate(kChunkBytes, kChunkAlignment);
            if (memory == nullptr)
            {
                say(World::Report::Error, "chunk allocation failed (%u bytes)", kChunkBytes);
                return nullptr;
            }
            chunk = new (memory) Chunk();
        }

        chunk->archetype = &archetype;
        chunk->capacity = archetype.layout.capacity;
        chunk->count = 0;
        ++chunk->version;
        return chunk;
    }

    /* Where a new row goes: the archetype's last chunk while it has room, a
       pooled or fresh one otherwise. Chunks stay dense — a removal moves the
       last row into the hole — so "has room" is one comparison. */
    Chunk* chunkWithRoom(Archetype& archetype)
    {
        if (!archetype.chunks.empty())
        {
            Chunk* last = archetype.chunks.back();
            if (last->count < last->capacity)
            {
                return last;
            }
        }

        Chunk* chunk = takeChunk(archetype);
        if (chunk == nullptr)
        {
            return nullptr;
        }
        archetype.chunks.push_back(chunk);
        return chunk;
    }

    /* An empty chunk goes back to the pool, except the archetype's only one:
       keeping it saves an allocation for the next entity of that archetype. */
    void releaseIfEmpty(Chunk* chunk)
    {
        Archetype& archetype = *chunk->archetype;
        if (chunk->count != 0U || archetype.chunks.size() <= 1U)
        {
            return;
        }
        archetype.chunks.erase(std::find(archetype.chunks.begin(), archetype.chunks.end(), chunk));
        pooled.push_back(chunk);
    }

    /* --- slots --- */

    /* Takes an index and a fresh generation; the entity is not alive until
       materialize places it. */
    Entity reserveSlot()
    {
        std::uint32_t index = 0;
        if (!freeSlots.empty())
        {
            index = freeSlots.back();
            freeSlots.pop_back();
        }
        else
        {
            index = static_cast<std::uint32_t>(slots.size());
            slots.emplace_back();
        }

        EntityRecord& record = slots[index];
        record.generation += 1U;
        if (record.generation == 0U)
        {
            record.generation = 1U;
        }
        record.reserved = true;
        return Entity{index, record.generation};
    }

    /* Places a reserved slot in the empty archetype and makes it alive. */
    bool materialize(Entity entity)
    {
        if (entity.index >= slots.size())
        {
            reportStale(entity, "create");
            return false;
        }
        EntityRecord& record = slots[entity.index];
        if (!record.reserved || record.generation != entity.generation || record.chunk != nullptr)
        {
            reportStale(entity, "create");
            return false;
        }

        Archetype* archetype = emptyArchetype();
        Chunk* chunk = archetype != nullptr ? chunkWithRoom(*archetype) : nullptr;
        if (chunk == nullptr)
        {
            return false;
        }

        entitiesOf(chunk)[chunk->count] = entity;
        record.chunk = chunk;
        record.row = chunk->count;
        record.reserved = false;
        chunk->count += 1U;
        ++chunk->version;
        ++archetype->entityCount;
        ++alive;
        return true;
    }

    /* Gives an unplayed reservation back; a slot that was materialized is left
       alone. */
    void releaseReservation(Entity entity)
    {
        if (entity.index >= slots.size())
        {
            return;
        }
        EntityRecord& record = slots[entity.index];
        if (!record.reserved || record.generation != entity.generation)
        {
            return;
        }
        record.reserved = false;
        freeSlots.push_back(entity.index);
    }

    /* --- rows --- */

    void* componentPointer(const EntityRecord& record, ComponentIndex component) const noexcept
    {
        return componentData(record.chunk, component) +
               static_cast<std::size_t>(record.row) * components[component].size;
    }

    /* Swap-removes one row, keeping the chunk dense: the last row takes its
       place, handles included, and the moved entity's slot is repointed. */
    void removeRow(Chunk* chunk, std::uint32_t row)
    {
        const Archetype& archetype = *chunk->archetype;
        const std::uint32_t last = chunk->count - 1U;

        if (row != last)
        {
            for (std::uint32_t slot = 0; slot < archetype.componentCount; ++slot)
            {
                const ComponentIndex component = archetype.components[slot];
                const std::size_t size = components[component].size;
                std::memcpy(componentData(chunk, component) + static_cast<std::size_t>(row) * size,
                            componentData(chunk, component) + static_cast<std::size_t>(last) * size,
                            size);
                componentTicks(chunk, component)[row] = componentTicks(chunk, component)[last];
            }

            const Entity moved = entitiesOf(chunk)[last];
            entitiesOf(chunk)[row] = moved;
            slots[moved.index].row = row;
        }

        chunk->count = last;
        --chunk->archetype->entityCount;
        ++chunk->version;
    }

    /* Moves the entity to `target`, copying the components both archetypes hold
       and zeroing the one being added (the caller writes its value afterwards).
       False when the target has no room to make. */
    bool moveTo(EntityRecord& record, Archetype& target, ComponentIndex added)
    {
        Chunk* from = record.chunk;
        const Archetype& source = *from->archetype;
        Chunk* to = chunkWithRoom(target);
        if (to == nullptr)
        {
            return false;
        }

        const std::uint32_t row = record.row;
        const std::uint32_t newRow = to->count;

        for (std::uint32_t slot = 0; slot < source.componentCount; ++slot)
        {
            const ComponentIndex component = source.components[slot];
            if ((target.mask & bitOf(component)) == 0U)
            {
                continue;
            }
            const std::size_t size = components[component].size;
            std::memcpy(componentData(to, component) + static_cast<std::size_t>(newRow) * size,
                        componentData(from, component) + static_cast<std::size_t>(row) * size,
                        size);
            componentTicks(to, component)[newRow] = componentTicks(from, component)[row];
        }

        if (added != kInvalidComponent)
        {
            const std::size_t size = components[added].size;
            std::memset(componentData(to, added) + static_cast<std::size_t>(newRow) * size, 0,
                        size);
            componentTicks(to, added)[newRow] = 0;
        }

        entitiesOf(to)[newRow] = entitiesOf(from)[row];
        to->count = newRow + 1U;
        ++target.entityCount;
        ++to->version;

        record.chunk = to;
        record.row = newRow;

        removeRow(from, row);
        releaseIfEmpty(from);
        return true;
    }

    /* Stamps the entity's tick for one component and the chunk's. */
    void stamp(EntityRecord& record, ComponentIndex component)
    {
        const ChangeTick tick = typeTicks[component].fetch_add(1U, std::memory_order_relaxed) + 1U;
        Chunk* chunk = record.chunk;
        componentTicks(chunk, component)[record.row] = tick;
        chunkTicksOf(chunk)[chunk->archetype->layout.tickSlots[component]] = tick;
    }

    void freeAll()
    {
        for (const std::unique_ptr<Archetype>& archetype : archetypes)
        {
            for (Chunk* chunk : archetype->chunks)
            {
                allocator->deallocate(chunk, kChunkBytes);
            }
            archetype->chunks.clear();
        }
        for (Chunk* chunk : pooled)
        {
            allocator->deallocate(chunk, kChunkBytes);
        }
        pooled.clear();
    }
};

} // namespace resonate::ecs::detail

#endif /* RESONATE_ECS_SRC_WORLD_INTERNAL_H */
