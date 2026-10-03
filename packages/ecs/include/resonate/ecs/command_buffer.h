#ifndef RESONATE_ECS_COMMAND_BUFFER_H
#define RESONATE_ECS_COMMAND_BUFFER_H

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
