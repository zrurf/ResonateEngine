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
 * parallel execution is in flight. A buffer discarded with unplayed creates
 * releases them and reports.
 *
 * Channels. SyncPoint is the default: the frame's two sync points play it,
 * and what is still recorded after the last one is reported and dropped.
 * NextFrame is the aftermath declaration: it plays at the start of the next
 * simulation step and survives the step's end otherwise. Which channel a
 * command lands on is decided when it is recorded — direct recording follows
 * setChannel, a section carries the channel it was opened with.
 *
 * Parallel recording. A `section` is one thread's private segment with its own
 * command and payload storage: sections of one buffer record concurrently and
 * touch nothing shared but the world's slot table (locked) and the buffer's
 * reserved-handle list. Closing folds the segment in; playback merges sections
 * by order key — sections with one key in open order — which is deterministic
 * when the keys are, the caller's parallel structure being what names them.
 * Direct recording and sections interleave freely: each keeps its place in the
 * order recording made.
 *
 * A domain records its own structural commands through `record`: the play
 * function runs at playback with the world, in record order with everything
 * else the buffer carries.
 */
class CommandBuffer
{
  private:
    /* One recorder's storage; defined in the implementation. A section holds
       its own segment by pointer, so recording never touches the (shaped
       under a lock) segment vector. */
    struct Segment;

  public:
    /* When what is recorded plays. */
    enum class Channel : std::uint8_t
    {
        SyncPoint = 0,
        NextFrame = 1,
    };

    explicit CommandBuffer(World& world);
    ~CommandBuffer();

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    /* --- direct recording: one thread, the current channel --- */

    /* Where the next direct commands go. The default is SyncPoint; switching
       is the explicit declaration the next-frame channel asks for. */
    void setChannel(Channel channel) noexcept;
    [[nodiscard]] Channel channel() const noexcept;

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

    /* --- parallel recording: sections --- */

    /* One thread's private segment. Sections are move-only, one per recording
       thread, and closed by destruction; recording through one has the same
       operations and the same validation as the buffer's own. `order_key`
       orders the merge at playback — a parallel-for passes its slice index, so
       the playback order follows the work's deterministic structure. */
    class Section
    {
      public:
        ~Section();

        Section(Section&& source) noexcept;
        Section(const Section&) = delete;
        Section& operator=(const Section&) = delete;

        [[nodiscard]] Entity create();
        void destroy(Entity entity);
        void add(Entity entity, ComponentIndex component, const void* value);
        void remove(Entity entity, ComponentIndex component);

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

        template <typename T> void add(Entity entity, const T& value)
        {
            add(entity, ComponentTraits<T>::index, &value);
        }

        template <typename T> void remove(Entity entity)
        {
            remove(entity, ComponentTraits<T>::index);
        }

        void record(PlayFn play, void* context, const void* payload, std::size_t size);

      private:
        friend class CommandBuffer;
        Section(CommandBuffer& buffer, std::uint64_t order_key, Channel channel);

        CommandBuffer* buffer_ = nullptr;
        Segment* segment_ = nullptr;
    };

    /* Opens a recording section on the current channel. */
    [[nodiscard]] Section section(std::uint64_t order_key = 0);

    /* --- inspection --- */

    /* The world the buffer records into. Read here it is the committed state:
       the records made earlier in this buffer are not in it yet. */
    [[nodiscard]] World& world() noexcept;

    [[nodiscard]] std::uint32_t commandCount() const noexcept;
    [[nodiscard]] std::uint32_t commandCount(Channel channel) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool empty(Channel channel) const noexcept;

    /* Drops everything and releases whatever the commands still reserved. */
    void clear();

    /* Drops one channel; the other keeps its commands and holds. */
    void clear(Channel channel);

  private:
    friend class World;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resonate::ecs

#endif /* RESONATE_ECS_COMMAND_BUFFER_H */
