#ifndef RESONATE_ECS_COMMAND_BUFFER_H
#define RESONATE_ECS_COMMAND_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "resonate/ecs/component.h"
#include "resonate/ecs/entity.h"

namespace resonate::ecs
{

class World;

/*
 * Recorded structural changes, played by the world at a frame point.
 *
 * A create reserves its slot immediately, so the handle it returns is final and
 * later commands may name it, but the entity does not exist for lookups or
 * queries until playback. Recording moves no data, so a system may record while
 * it iterates; playback is where structure moves, and it is refused while
 * parallel execution is in flight.
 *
 * One thread records a buffer and the world plays it once. A buffer discarded
 * with unplayed creates releases them and reports. Per-thread segments for
 * recording from parallel work arrive with the frame arena.
 *
 * A domain records its own structural commands through `record`: the play
 * function runs at playback with the world, in record order with everything
 * else the buffer carries. That is how a tree edit or another domain-specific
 * operation stays inside law 2 without the ECS knowing what it means.
 */
class CommandBuffer
{
  public:
    explicit CommandBuffer(World& world);
    ~CommandBuffer();

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    /* Reserves a handle now; the entity becomes alive when the buffer plays. */
    [[nodiscard]] Entity create();

    /* `create` plus one `add` per component. */
    template <typename... T> Entity spawn(const T&... values)
    {
        const Entity entity = create();
        if (entity.valid())
        {
            (add<T>(entity, values), ...);
        }
        return entity;
    }

    void destroy(Entity entity);

    /* The value is copied into the buffer, so the caller's memory is free on
       return. A null value records a zero-initialised component. */
    void add(Entity entity, ComponentIndex component, const void* value);
    void remove(Entity entity, ComponentIndex component);

    template <typename T> void add(Entity entity, const T& value)
    {
        add(entity, ComponentTraits<T>::index, &value);
    }

    template <typename T> void remove(Entity entity)
    {
        remove(entity, ComponentTraits<T>::index);
    }

    /* A structural command the recorder defines. `play` runs at playback, in
       record order; the payload is copied into the buffer (aligned for any
       scalar), so the recorder's memory is free on return, while `context` is
       borrowed and must outlive playback. The play function reports what it
       refuses through the world. */
    using PlayFn = void (*)(void* context, World& world, const void* payload);
    void record(PlayFn play, void* context, const void* payload, std::size_t size);

    /* The world the buffer records into. Read here it is the committed state:
       the records made earlier in this buffer are not in it yet. */
    [[nodiscard]] World& world() noexcept;

    [[nodiscard]] std::uint32_t commandCount() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    /* Drops the commands and releases whatever they still reserved. */
    void clear();

  private:
    friend class World;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resonate::ecs

#endif /* RESONATE_ECS_COMMAND_BUFFER_H */
