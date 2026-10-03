#ifndef RESONATE_ECS_WORLD_H
#define RESONATE_ECS_WORLD_H

#include <cstdint>
#include <memory>

#include "resonate/core/allocator.h"
#include "resonate/ecs/component.h"
#include "resonate/ecs/entity.h"
#include "resonate/ecs/query.h"

/*
 * Archetype storage: the authoritative game state.
 *
 * An entity is a handle into a slot table; its components live in 16KB chunks
 * that hold one archetype each — the set of components the entity has. A
 * structural change (create, destroy, add, remove) either moves the entity
 * along an archetype edge, copying the components it keeps, or allocates the
 * row it lands in. Chunks are laid out per archetype: the entity handles, a
 * chunk-level tick per component type, then per component a dense array and a
 * per-entity tick array.
 *
 * Storage and playback layer: structural changes belong in the entity command
 * buffer, and the buffer, tests and benchmarks are what call the primitives
 * below. Any structural change is refused while parallel execution is in
 * flight.
 *
 * Handle discipline: every access validates the generation and reports the
 * failure; a stale or dead handle fails rather than touching another entity's
 * data. Pointers into a chunk (component addresses) are only valid until the
 * next structural change.
 */

namespace resonate::ecs
{

namespace detail
{
struct WorldImpl;
}

using ChangeTick = std::uint32_t;

/* Wrap-safe tick order: true when `a` was stamped after `b`. Ticks are u32
   counters, so the comparison is the signed distance, not the numeric order. */
[[nodiscard]] constexpr bool tickAfter(ChangeTick a, ChangeTick b) noexcept
{
    return static_cast<std::int32_t>(a - b) > 0;
}

class CommandBuffer;

class World
{
  public:
    /* Where refusals and failures are reported. The level is this package's own
       scale, not the module log: the ECS sits below the plugin ABI and must not
       include it. */
    enum class Report : std::int32_t
    {
        Info = 0,
        Warning = 1,
        Error = 2,
    };

    using ReportFn = void (*)(void* user_data, Report level, const char* message);

    explicit World(Allocator& allocator);
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    /* Empty restores the default, which writes to stderr like the host's. */
    void setReportSink(void* user_data, ReportFn sink);

    /* --- component types --- */

    /* Registers ComponentTraits<T> in this world: its layout is recorded here,
       and its process-wide index is assigned the first time any world registers
       the type. Registering the same type again, in this world or another,
       returns the same index. A second type claiming a registered name with a
       different layout, or a registration past the last mask bit, is reported
       and returns kInvalidComponent. */
    template <typename T> ComponentIndex registerComponent()
    {
        static_assert(ComponentTraits<T>::name != nullptr,
                      "a component type must name itself: specialise ComponentTraits<T>");
        const ComponentIndex index = registerComponent(
            ComponentTraits<T>::name, ComponentTraits<T>::size, ComponentTraits<T>::alignment);
        if (index != kInvalidComponent)
        {
            ComponentTraits<T>::index = index;
        }
        return index;
    }

    /* The index of a type this world has registered, or kInvalidComponent. */
    [[nodiscard]] ComponentIndex findComponent(const char* name) const noexcept;

    /* The number of component types this world registered. */
    [[nodiscard]] std::uint32_t componentTypeCount() const noexcept;

    /* --- entities (structural: the playback layer, see the header comment) --- */

    /* An invalid handle (generation 0) when the storage could not grow. */
    [[nodiscard]] Entity create();
    void destroy(Entity entity);
    [[nodiscard]] bool alive(Entity entity) const noexcept;
    [[nodiscard]] std::uint32_t entityCount() const noexcept;

    /* --- components on an entity --- */

    [[nodiscard]] void* get(Entity entity, ComponentIndex component) noexcept;
    [[nodiscard]] const void* get(Entity entity, ComponentIndex component) const noexcept;
    [[nodiscard]] bool has(Entity entity, ComponentIndex component) const noexcept;

    /* Structural: migrates the entity to the archetype that has this component
       and returns the component, or null when the entity is not alive, the
       component index is not registered, or the resulting archetype cannot hold
       one entity per chunk. A component the entity already has is returned
       unchanged. A null `value` zero-initialises it. */
    void* add(Entity entity, ComponentIndex component, const void* value);

    /* Structural: migrates the entity to the archetype without the component.
       Removing one the entity does not have is a no-op; false when the entity
       is not alive. */
    bool remove(Entity entity, ComponentIndex component);

    template <typename T> [[nodiscard]] T* get(Entity entity) noexcept
    {
        return static_cast<T*>(get(entity, ComponentTraits<T>::index));
    }

    template <typename T> [[nodiscard]] const T* get(Entity entity) const noexcept
    {
        return static_cast<const T*>(get(entity, ComponentTraits<T>::index));
    }

    template <typename T> [[nodiscard]] bool has(Entity entity) const noexcept
    {
        return has(entity, ComponentTraits<T>::index);
    }

    template <typename T> T* add(Entity entity, const T& value)
    {
        return static_cast<T*>(add(entity, ComponentTraits<T>::index, &value));
    }

    template <typename T> bool remove(Entity entity)
    {
        return remove(entity, ComponentTraits<T>::index);
    }

    /* --- queries --- */

    /* A pre-registered query group. The returned query is bound to this world
       and catches up with new archetypes as they appear; a description naming a
       component this world did not register yields an invalid query (valid() is
       false) and reports. */
    [[nodiscard]] Query createQuery(const QueryDesc& desc);

    /* --- command buffer --- */

    /* Applies a recorded buffer, in record order. Refused as a whole while
       parallel execution is in flight (the immediate channel's check); the
       buffer keeps its commands so the caller can play it when the frame's
       barrier arrives. */
    void play(CommandBuffer& buffer);

    /* --- change observers --- */

    /* Called for every entity a component arrives on, is stamped on or leaves,
       from a played command or a direct write alike, with the world in its
       post-change state; in registration order, on the thread that made the
       change. A callback must not change structure — a structural call from it
       is refused and reported. Entity destruction is not announced here. */
    using ChangeObserver = void (*)(void* user_data, World& world, Entity entity,
                                    ComponentIndex component);

    /* False, with a report, for a component this world did not register or an
       observer that is null. */
    bool addObserver(ComponentIndex component, void* user_data, ChangeObserver observer);

    /* Dropping one that was never added is a no-op. */
    void removeObserver(ComponentIndex component, void* user_data, ChangeObserver observer);

    template <typename T> bool addObserver(void* user_data, ChangeObserver observer)
    {
        return addObserver(ComponentTraits<T>::index, user_data, observer);
    }

    template <typename T> void removeObserver(void* user_data, ChangeObserver observer)
    {
        removeObserver(ComponentTraits<T>::index, user_data, observer);
    }

    /* --- change detection --- */

    /* Stamps the entity's tick for the component and advances the type's global
       tick. A write through a raw chunk view cannot know which entities it
       touched, so its owner marks what it wrote: one entity for a targeted
       write, the whole chunk for a dense scan. */
    void markChanged(Entity entity, ComponentIndex component);

    template <typename T> void markChanged(Entity entity)
    {
        markChanged(entity, ComponentTraits<T>::index);
    }

    /* The newest stamp of the component type, for "has anything of this type
       changed" checks; per-entity precision is `changedSince`. */
    [[nodiscard]] ChangeTick componentTick(ComponentIndex component) const noexcept;

    /* Whether this entity's component was stamped after `tick`. False for a
       component the entity does not have. */
    [[nodiscard]] bool changedSince(Entity entity, ComponentIndex component,
                                    ChangeTick tick) const noexcept;

    template <typename T> [[nodiscard]] ChangeTick componentTick() const noexcept
    {
        return componentTick(ComponentTraits<T>::index);
    }

    template <typename T>
    [[nodiscard]] bool changedSince(Entity entity, ChangeTick tick) const noexcept
    {
        return changedSince(entity, ComponentTraits<T>::index, tick);
    }

    /* --- parallel execution --- */

    /* Marks a region that may run on several threads at once. Structural
       changes are refused and reported while one is open; a query's parallel
       iteration opens it, and command-buffer playback checks it. */
    class ParallelScope
    {
      public:
        explicit ParallelScope(World& world) noexcept;
        ~ParallelScope();

        ParallelScope(const ParallelScope&) = delete;
        ParallelScope& operator=(const ParallelScope&) = delete;

      private:
        World* world_;
    };

    [[nodiscard]] bool inParallelExecution() const noexcept;

    /* --- diagnostics --- */

    [[nodiscard]] std::uint32_t archetypeCount() const noexcept;

    /* Chunks attached to an archetype, and empty chunks held for reuse. */
    [[nodiscard]] std::uint32_t liveChunkCount() const noexcept;
    [[nodiscard]] std::uint32_t pooledChunkCount() const noexcept;

  private:
    friend class CommandBuffer;
    friend class Query;

    /* Slot reservation for a recorded create: the handle is final at record
       time, the entity is not alive until playback. */
    [[nodiscard]] Entity reserveEntity();
    void releaseEntity(Entity entity);

    /* The erased registration the template above funnels into. */
    ComponentIndex registerComponent(const char* name, std::uint32_t size, std::uint32_t alignment);

    /* Calls the observers of one component, in registration order. */
    void notifyChange(Entity entity, ComponentIndex component);

    void enterParallel() noexcept;
    void leaveParallel() noexcept;

    std::unique_ptr<detail::WorldImpl> impl_;
};

} // namespace resonate::ecs

#endif /* RESONATE_ECS_WORLD_H */
