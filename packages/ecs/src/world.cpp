#include "world_internal.h"

#include <xxhash.h>

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

void World::report(Report level, const char* format, ...)
{
    char message[256] = {};
    std::va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    /* Through the %s, so a message a caller formatted cannot be read as a
       format string again. */
    impl_->say(level, "%s", message);
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

bool World::addObserver(ComponentIndex component, void* user_data, ChangeObserver observer)
{
    if (!impl_->requireType(component, "addObserver"))
    {
        return false;
    }
    if (observer == nullptr)
    {
        impl_->say(Report::Error, "addObserver: observer for component %u is null", component);
        return false;
    }

    impl_->observers[component].push_back(detail::WorldImpl::ObserverEntry{user_data, observer});
    return true;
}

void World::removeObserver(ComponentIndex component, void* user_data, ChangeObserver observer)
{
    if (!impl_->hasType(component))
    {
        return;
    }

    std::vector<detail::WorldImpl::ObserverEntry>& entries = impl_->observers[component];
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [user_data, observer](const detail::WorldImpl::ObserverEntry& entry)
                       { return entry.observer == observer && entry.user_data == user_data; }),
        entries.end());
}

void World::notifyChange(Entity entity, ComponentIndex component)
{
    const std::vector<detail::WorldImpl::ObserverEntry>& entries = impl_->observers[component];
    if (entries.empty())
    {
        return;
    }

    ++impl_->dispatching;
    for (const detail::WorldImpl::ObserverEntry& entry : entries)
    {
        entry.observer(entry.user_data, *this, entity, component);
    }
    --impl_->dispatching;
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

    /* A handle that names a recorded-but-unplayed create cancels the record
       instead of failing: the entity was never alive, so there is no row to
       remove and the reservation is what dies. */
    {
        std::lock_guard<std::mutex> lock(impl_->slotMutex);
        if (entity.index < impl_->slots.size() && impl_->slots[entity.index].reserved &&
            impl_->slots[entity.index].generation == entity.generation)
        {
            impl_->slots[entity.index].reserved = false;
            impl_->freeSlots.push_back(entity.index);
            return;
        }
    }

    EntityRecord* record = impl_->recordOf(entity, "destroy");
    if (record == nullptr)
    {
        return;
    }

    /* The blobs are the entity's, so they die with it — while the record still
       names them. */
    impl_->reclaimBlobs(*record);

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
    std::lock_guard<std::mutex> lock(impl_->slotMutex);
    if (entity.index >= impl_->slots.size())
    {
        return false;
    }
    const EntityRecord& record = impl_->slots[entity.index];
    return record.chunk != nullptr && record.generation == entity.generation;
}

std::uint32_t World::entityCount() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->slotMutex);
    return impl_->alive;
}

void* World::get(Entity entity, ComponentIndex component) noexcept
{
    if (!impl_->requireType(component, "get"))
    {
        return nullptr;
    }
    detail::WorldImpl::SlotLocation location;
    if (!impl_->locate(entity, location, "get"))
    {
        return nullptr;
    }
    if ((location.chunk->archetype->mask & bitOf(component)) == 0U)
    {
        return nullptr;
    }
    return impl_->componentPointer(location.chunk, location.row, component);
}

const void* World::get(Entity entity, ComponentIndex component) const noexcept
{
    return const_cast<World*>(this)->get(entity, component);
}

bool World::has(Entity entity, ComponentIndex component) const noexcept
{
    if (!impl_->hasType(component))
    {
        return false;
    }
    detail::WorldImpl::SlotLocation location;
    if (!impl_->locate(entity, location, nullptr))
    {
        return false;
    }
    return (location.chunk->archetype->mask & bitOf(component)) != 0U;
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
        return impl_->componentPointer(record->chunk, record->row, component);
    }

    Archetype* target = impl_->edge(source, component, true);
    if (target == nullptr || !impl_->moveTo(*record, *target, component))
    {
        return nullptr;
    }

    void* pointer = impl_->componentPointer(record->chunk, record->row, component);
    if (value != nullptr)
    {
        std::memcpy(pointer, value, impl_->components[component].size);
    }

    /* The component arrived with a value: that is a change to announce. */
    impl_->stamp(record->chunk, record->row, component);
    notifyChange(entity, component);
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

    /* Gone is a change like arrived is: with no stamp left on the entity, the
       observers are what announces it. */
    notifyChange(entity, component);
    return true;
}

BlobHandle World::createBlob(Entity owner, std::size_t size, const void* data)
{
    if (!impl_->structural())
    {
        return BlobHandle{};
    }
    EntityRecord* record = impl_->recordOf(owner, "createBlob");
    if (record == nullptr)
    {
        return BlobHandle{};
    }

    std::uint32_t index = 0;
    if (!impl_->freeBlobs.empty())
    {
        index = impl_->freeBlobs.back();
        impl_->freeBlobs.pop_back();
    }
    else
    {
        index = static_cast<std::uint32_t>(impl_->blobs.size());
        impl_->blobs.emplace_back();
    }

    detail::BlobRecord& blob = impl_->blobs[index];
    blob.generation += 1U;
    if (blob.generation == 0U)
    {
        blob.generation = 1U;
    }

    if (size != 0)
    {
        blob.bytes = impl_->allocator->allocate(size, detail::kBlobAlignment);
        if (blob.bytes == nullptr)
        {
            impl_->say(Report::Error, "createBlob: allocation of %zu bytes failed", size);
            impl_->freeBlobs.push_back(index);
            return BlobHandle{};
        }
        if (data != nullptr)
        {
            std::memcpy(blob.bytes, data, size);
        }
        else
        {
            std::memset(blob.bytes, 0, size);
        }
    }

    blob.size = size;
    blob.live = true;
    blob.owner = owner;
    blob.next = record->firstBlob;
    record->firstBlob = index;
    ++impl_->liveBlobs;
    return BlobHandle{index, blob.generation};
}

bool World::resizeBlob(BlobHandle handle, std::size_t size)
{
    if (!impl_->structural())
    {
        return false;
    }
    detail::BlobRecord* blob = impl_->blobOf(handle, "resizeBlob");
    if (blob == nullptr || size == blob->size)
    {
        return blob != nullptr;
    }

    void* bytes = nullptr;
    if (size != 0)
    {
        bytes = impl_->allocator->allocate(size, detail::kBlobAlignment);
        if (bytes == nullptr)
        {
            impl_->say(Report::Error, "resizeBlob: allocation of %zu bytes failed", size);
            return false;
        }
        const std::size_t kept = size < blob->size ? size : blob->size;
        if (kept != 0)
        {
            std::memcpy(bytes, blob->bytes, kept);
        }
        if (size > blob->size)
        {
            std::memset(static_cast<std::byte*>(bytes) + blob->size, 0, size - blob->size);
        }
    }

    if (blob->bytes != nullptr)
    {
        impl_->allocator->deallocate(blob->bytes, blob->size);
    }
    blob->bytes = bytes;
    blob->size = size;
    return true;
}

bool World::destroyBlob(BlobHandle handle)
{
    if (!impl_->structural())
    {
        return false;
    }
    if (impl_->blobOf(handle, "destroyBlob") == nullptr)
    {
        return false;
    }
    impl_->releaseBlob(handle.index);
    return true;
}

void* World::blobData(BlobHandle handle) noexcept
{
    detail::BlobRecord* blob = impl_->blobOf(handle, "blobData");
    return blob != nullptr ? blob->bytes : nullptr;
}

const void* World::blobData(BlobHandle handle) const noexcept
{
    return const_cast<World*>(this)->blobData(handle);
}

std::size_t World::blobSize(BlobHandle handle) const noexcept
{
    const detail::BlobRecord* blob = impl_->findBlob(handle);
    return blob != nullptr ? blob->size : 0;
}

bool World::alive(BlobHandle handle) const noexcept
{
    return impl_->findBlob(handle) != nullptr;
}

void World::markChanged(Entity entity, ComponentIndex component)
{
    if (!impl_->requireType(component, "markChanged"))
    {
        return;
    }
    detail::WorldImpl::SlotLocation location;
    if (!impl_->locate(entity, location, "markChanged"))
    {
        return;
    }
    if ((location.chunk->archetype->mask & bitOf(component)) == 0U)
    {
        impl_->say(Report::Error, "markChanged: entity %u:%u has no component %u", entity.index,
                   entity.generation, component);
        return;
    }
    impl_->stamp(location.chunk, location.row, component);
    notifyChange(entity, component);
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
    detail::WorldImpl::SlotLocation location;
    if (!impl_->locate(entity, location, "changedSince"))
    {
        return false;
    }
    if ((location.chunk->archetype->mask & bitOf(component)) == 0U)
    {
        return false;
    }
    return tickAfter(componentTicks(location.chunk, component)[location.row], tick);
}

void World::setSeed(std::uint64_t seed) noexcept
{
    impl_->reseed(seed);
}

Rng* World::rngStream(const char* name)
{
    return impl_->rngStream(name);
}

World::ParallelScope::ParallelScope(World& world) noexcept : world_(&world)
{
    world_->enterParallel();
}World::ParallelScope::~ParallelScope()
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

std::uint32_t World::blobCount() const noexcept
{
    return impl_->liveBlobs;
}

} // namespace resonate::ecs
