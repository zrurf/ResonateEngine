#include "world_internal.h"

namespace resonate::ecs
{

/* The storage's internals live in the package-private header. */
using detail::Archetype;
using detail::bitOf;
using detail::Chunk;
using detail::ComponentType;
using detail::ComponentUniverse;
using detail::componentUniverse;
using detail::EntityRecord;
using detail::isPowerOfTwo;
using detail::kChunkAlignment;

World::World(Allocator& allocator) : impl_(std::make_unique<detail::WorldImpl>(allocator))
{
}

World::~World()
{
    impl_->freeAll();
}

void World::setReportSink(void* user_data, ReportFn sink)
{
    impl_->report_user = user_data;
    impl_->report = sink;
}

ComponentIndex World::registerComponent(const char* name, std::uint32_t size,
                                        std::uint32_t alignment)
{
    if (name == nullptr || *name == '\0')
    {
        impl_->say(Report::Error, "a component type must be named");
        return kInvalidComponent;
    }
    if (size == 0U)
    {
        impl_->say(Report::Error, "component '%s' has no size", name);
        return kInvalidComponent;
    }
    if (!isPowerOfTwo(alignment) || alignment > kChunkAlignment || (size % alignment) != 0U)
    {
        impl_->say(Report::Error, "component '%s' has an unusable layout (%u bytes, alignment %u)",
                   name, size, alignment);
        return kInvalidComponent;
    }

    /* The index is process-wide: the first world to register a type assigns it,
       and every later world reuses it. A name already taken by a different
       layout is a schema conflict; the two are not the same component. */
    ComponentIndex index = kInvalidComponent;
    {
        ComponentUniverse& universe = componentUniverse();
        const std::lock_guard<std::mutex> lock(universe.mutex);
        for (std::uint32_t candidate = 0; candidate < universe.count; ++candidate)
        {
            if (std::strcmp(universe.types[candidate].name, name) == 0)
            {
                if (universe.types[candidate].size != size ||
                    universe.types[candidate].alignment != alignment)
                {
                    impl_->say(Report::Error,
                               "component '%s' is already registered with a different layout",
                               name);
                    return kInvalidComponent;
                }
                index = static_cast<ComponentIndex>(candidate);
                break;
            }
        }

        if (index == kInvalidComponent)
        {
            if (universe.count == kMaxComponentTypes)
            {
                impl_->say(Report::Error, "component '%s' exceeds the %u type limit of one mask",
                           name, kMaxComponentTypes);
                return kInvalidComponent;
            }
            universe.types[universe.count] = ComponentType{name, size, alignment};
            index = static_cast<ComponentIndex>(universe.count++);
        }
    }

    /* Recorded in the world that asked: the layout is what a chunk of this
       world's archetypes is built from. */
    impl_->registerType(index, name, size, alignment);
    return index;
}

ComponentIndex World::findComponent(const char* name) const noexcept
{
    return impl_->findComponent(name);
}

Query World::createQuery(const QueryDesc& desc)
{
    return Query(*this, desc);
}

std::uint32_t World::componentTypeCount() const noexcept
{
    return impl_->registeredCount;
}

Entity World::create()
{
    if (!impl_->structural())
    {
        return Entity{};
    }

    const Entity entity = impl_->reserveSlot();
    if (!impl_->materialize(entity))
    {
        impl_->releaseReservation(entity);
        return Entity{};
    }
    return entity;
}

Entity World::reserveEntity()
{
    return impl_->reserveSlot();
}

void World::releaseEntity(Entity entity)
{
    impl_->releaseReservation(entity);
}

void World::destroy(Entity entity)
{
    if (!impl_->structural())
    {
        return;
    }

    EntityRecord* record = impl_->recordOf(entity, "destroy");
    if (record == nullptr)
    {
        return;
    }

    Chunk* chunk = record->chunk;
    impl_->removeRow(chunk, record->row);
    impl_->releaseIfEmpty(chunk);

    record->chunk = nullptr;
    record->row = 0;
    impl_->freeSlots.push_back(entity.index);
    --impl_->alive;
}

bool World::alive(Entity entity) const noexcept
{
    if (entity.index >= impl_->slots.size())
    {
        return false;
    }
    const EntityRecord& record = impl_->slots[entity.index];
    return record.chunk != nullptr && record.generation == entity.generation;
}

std::uint32_t World::entityCount() const noexcept
{
    return impl_->alive;
}

void* World::get(Entity entity, ComponentIndex component) noexcept
{
    if (!impl_->requireType(component, "get"))
    {
        return nullptr;
    }
    EntityRecord* record = impl_->recordOf(entity, "get");
    if (record == nullptr || (record->chunk->archetype->mask & bitOf(component)) == 0U)
    {
        return nullptr;
    }
    return impl_->componentPointer(*record, component);
}

const void* World::get(Entity entity, ComponentIndex component) const noexcept
{
    return const_cast<World*>(this)->get(entity, component);
}

bool World::has(Entity entity, ComponentIndex component) const noexcept
{
    if (!impl_->hasType(component) || entity.index >= impl_->slots.size())
    {
        return false;
    }
    const EntityRecord& record = impl_->slots[entity.index];
    return record.chunk != nullptr && record.generation == entity.generation &&
           (record.chunk->archetype->mask & bitOf(component)) != 0U;
}

void* World::add(Entity entity, ComponentIndex component, const void* value)
{
    if (!impl_->structural())
    {
        return nullptr;
    }
    if (!impl_->requireType(component, "add"))
    {
        return nullptr;
    }
    EntityRecord* record = impl_->recordOf(entity, "add");
    if (record == nullptr)
    {
        return nullptr;
    }

    Archetype& source = *record->chunk->archetype;
    if ((source.mask & bitOf(component)) != 0U)
    {
        return impl_->componentPointer(*record, component);
    }

    Archetype* target = impl_->edge(source, component, true);
    if (target == nullptr || !impl_->moveTo(*record, *target, component))
    {
        return nullptr;
    }

    void* pointer = impl_->componentPointer(*record, component);
    if (value != nullptr)
    {
        std::memcpy(pointer, value, impl_->components[component].size);
    }

    /* The component arrived with a value: that is a change to announce. */
    impl_->stamp(*record, component);
    return pointer;
}

bool World::remove(Entity entity, ComponentIndex component)
{
    if (!impl_->structural())
    {
        return false;
    }
    if (!impl_->requireType(component, "remove"))
    {
        return false;
    }
    EntityRecord* record = impl_->recordOf(entity, "remove");
    if (record == nullptr)
    {
        return false;
    }

    Archetype& source = *record->chunk->archetype;
    if ((source.mask & bitOf(component)) == 0U)
    {
        return false;
    }

    Archetype* target = impl_->edge(source, component, false);
    if (target == nullptr || !impl_->moveTo(*record, *target, kInvalidComponent))
    {
        return false;
    }
    return true;
}

void World::markChanged(Entity entity, ComponentIndex component)
{
    if (!impl_->requireType(component, "markChanged"))
    {
        return;
    }
    EntityRecord* record = impl_->recordOf(entity, "markChanged");
    if (record == nullptr)
    {
        return;
    }
    if ((record->chunk->archetype->mask & bitOf(component)) == 0U)
    {
        impl_->say(Report::Error, "markChanged: entity %u:%u has no component %u", entity.index,
                   entity.generation, component);
        return;
    }
    impl_->stamp(*record, component);
}

ChangeTick World::componentTick(ComponentIndex component) const noexcept
{
    if (!impl_->hasType(component))
    {
        return 0;
    }
    return impl_->typeTicks[component].load(std::memory_order_relaxed);
}

bool World::changedSince(Entity entity, ComponentIndex component, ChangeTick tick) const noexcept
{
    if (!impl_->hasType(component))
    {
        return false;
    }
    const EntityRecord* record = impl_->recordOf(entity, "changedSince");
    if (record == nullptr || (record->chunk->archetype->mask & bitOf(component)) == 0U)
    {
        return false;
    }
    return tickAfter(componentTicks(record->chunk, component)[record->row], tick);
}

World::ParallelScope::ParallelScope(World& world) noexcept : world_(&world)
{
    world_->enterParallel();
}

World::ParallelScope::~ParallelScope()
{
    world_->leaveParallel();
}

bool World::inParallelExecution() const noexcept
{
    return impl_->parallelDepth.load(std::memory_order_relaxed) != 0U;
}

void World::enterParallel() noexcept
{
    impl_->parallelDepth.fetch_add(1U, std::memory_order_relaxed);
}

void World::leaveParallel() noexcept
{
    impl_->parallelDepth.fetch_sub(1U, std::memory_order_relaxed);
}

std::uint32_t World::archetypeCount() const noexcept
{
    return static_cast<std::uint32_t>(impl_->archetypes.size());
}

std::uint32_t World::liveChunkCount() const noexcept
{
    std::uint32_t count = 0;
    for (const std::unique_ptr<Archetype>& archetype : impl_->archetypes)
    {
        count += static_cast<std::uint32_t>(archetype->chunks.size());
    }
    return count;
}

std::uint32_t World::pooledChunkCount() const noexcept
{
    return static_cast<std::uint32_t>(impl_->pooled.size());
}

} // namespace resonate::ecs
