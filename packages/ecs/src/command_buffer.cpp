#include <resonate/ecs/command_buffer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "world_internal.h"

namespace resonate::ecs
{

/* One recorder's storage: the inline segment carries the direct commands, a
   section its own. A segment's address is stable for the buffer's life, so
   recording never touches the segment vector; only opening a section or
   playback shapes it, and those share one mutex. */
struct CommandBuffer::Segment
{
    enum class Kind : std::uint8_t
    {
        Create,
        Destroy,
        Add,
        Remove,
        Recorded,
    };

    struct Command
    {
        Kind kind = Kind::Create;
        Entity entity{};
        ComponentIndex component = kInvalidComponent;
        std::uint32_t payload = 0; /* offset into `bytes` */
        std::uint32_t size = 0;
        CommandBuffer::PlayFn play = nullptr; /* Recorded only */
        void* context = nullptr;              /* Recorded only */
    };

    std::vector<Command> commands;
    std::vector<std::byte> bytes;

    Channel channel = Channel::SyncPoint;
    bool is_section = false;
    std::uint64_t order_key = 0;
    std::uint32_t sequence = 0;
};

struct CommandBuffer::Impl
{
    World* world = nullptr;
    detail::WorldImpl* impl = nullptr;

    /* Where direct recording lands. */
    Channel channel = Channel::SyncPoint;

    /* Chronological by creation: the inline segment first, a section segment
       when it opens, a fresh inline segment when direct recording follows an
       opened section — so playback keeps the order recording made. */
    std::vector<std::unique_ptr<Segment>> segments;
    std::mutex segment_mutex; /* the vector's shape and the open-section count */
    std::uint32_t section_sequence = 0;
    std::uint32_t open_sections = 0;

    /* Entities this buffer reserved and has not handed to the world yet, from
       every segment. Recording runs on many threads now, so both the scan and
       the append share one mutex. */
    struct Hold
    {
        Entity entity;
        Channel channel;
    };
    std::vector<Hold> holds;
    mutable std::mutex hold_mutex;

    /* The segment direct recording lands on: the last one, unless it is a
       section's or was opened for another channel — recording after either
       starts a fresh inline segment, so the merge keeps the order recording
       made and each command lands on the channel it was recorded for. */
    Segment& inlineSegment()
    {
        std::lock_guard<std::mutex> lock(segment_mutex);
        if (segments.empty() || segments.back()->is_section ||
            segments.back()->channel != channel)
        {
            segments.push_back(std::make_unique<Segment>());
            segments.back()->channel = channel;
        }
        return *segments.back();
    }

    /* An entity a command may name: alive now, or created by this buffer and
       not yet played. */
    bool names(Entity entity) const
    {
        {
            std::lock_guard<std::mutex> lock(hold_mutex);
            for (const Hold& hold : holds)
            {
                if (hold.entity == entity)
                {
                    return true;
                }
            }
        }
        return world->alive(entity);
    }

    bool requireEntity(Entity entity, const char* what) const
    {
        if (!names(entity))
        {
            impl->say(World::Report::Error,
                      "%s: entity %u:%u is neither alive nor created by this buffer", what,
                      entity.index, entity.generation);
            return false;
        }
        return true;
    }

    /* --- recording into one segment --- */

    Entity createInto(Segment& segment)
    {
        const Entity entity = world->reserveEntity();
        if (!entity.valid())
        {
            return entity;
        }

        segment.commands.push_back(Segment::Command{Segment::Kind::Create, entity});
        std::lock_guard<std::mutex> lock(hold_mutex);
        holds.push_back({entity, segment.channel});
        return entity;
    }

    void destroyInto(Segment& segment, Entity entity)
    {
        if (!requireEntity(entity, "destroy"))
        {
            return;
        }
        segment.commands.push_back(Segment::Command{Segment::Kind::Destroy, entity});
    }

    void addInto(Segment& segment, Entity entity, ComponentIndex component, const void* value)
    {
        if (!impl->requireType(component, "add"))
        {
            return;
        }
        if (!requireEntity(entity, "add"))
        {
            return;
        }

        const std::uint32_t size = impl->components[component].size;
        const std::uint32_t offset = static_cast<std::uint32_t>(segment.bytes.size());
        segment.bytes.resize(segment.bytes.size() + size);
        if (value != nullptr)
        {
            std::memcpy(segment.bytes.data() + offset, value, size);
        }
        else
        {
            std::memset(segment.bytes.data() + offset, 0, size);
        }

        segment.commands.push_back(
            Segment::Command{Segment::Kind::Add, entity, component, offset, size});
    }

    void removeInto(Segment& segment, Entity entity, ComponentIndex component)
    {
        if (!impl->requireType(component, "remove"))
        {
            return;
        }
        if (!requireEntity(entity, "remove"))
        {
            return;
        }
        segment.commands.push_back(Segment::Command{Segment::Kind::Remove, entity, component});
    }

    void recordInto(Segment& segment, PlayFn play, void* context, const void* payload,
                    std::size_t size)
    {
        if (play == nullptr || (size != 0 && payload == nullptr))
        {
            impl->say(World::Report::Error,
                      "record: a command needs a play function and payload");
            return;
        }

        /* Aligned for any scalar the play function may read out of it, which
           the component payloads below do not need — they are memcpy'd, not
           read. */
        const std::uint32_t aligned =
            detail::alignUp(static_cast<std::uint32_t>(segment.bytes.size()),
                            static_cast<std::uint32_t>(alignof(std::max_align_t)));
        segment.bytes.resize(aligned + size);
        if (size != 0)
        {
            std::memcpy(segment.bytes.data() + aligned, payload, size);
        }

        segment.commands.push_back(Segment::Command{Segment::Kind::Recorded, Entity{},
                                                    kInvalidComponent, aligned,
                                                    static_cast<std::uint32_t>(size), play,
                                                    context});
    }

    /* --- the merge and the drop --- */

    /* The merged order of one channel's segments: chronological, with each
       contiguous run of section segments sorted by order key, then open
       sequence. Direct commands bracket the runs the way recording made them. */
    std::vector<Segment*> mergeOrder(Channel playing)
    {
        std::vector<Segment*> order;
        std::vector<Segment*> run;

        const auto flush = [&run, &order]()
        {
            std::sort(run.begin(), run.end(),
                      [](const Segment* a, const Segment* b)
                      {
                          if (a->order_key != b->order_key)
                          {
                              return a->order_key < b->order_key;
                          }
                          return a->sequence < b->sequence;
                      });
            order.insert(order.end(), run.begin(), run.end());
            run.clear();
        };

        for (std::unique_ptr<Segment>& segment : segments)
        {
            if (segment->channel != playing)
            {
                continue;
            }
            if (!segment->is_section && !run.empty())
            {
                flush();
            }
            if (segment->is_section)
            {
                run.push_back(segment.get());
                continue;
            }
            order.push_back(segment.get());
        }
        if (!run.empty())
        {
            flush();
        }
        return order;
    }

    /* Drops one channel's segments and resolves its holds: after a play the
       entities are alive, after a discard they give their reservations back. */
    void releaseChannel(Channel playing, bool played)
    {
        std::vector<std::unique_ptr<Segment>> kept;
        for (std::unique_ptr<Segment>& segment : segments)
        {
            if (segment->channel != playing)
            {
                kept.push_back(std::move(segment));
            }
        }
        segments = std::move(kept);

        std::lock_guard<std::mutex> lock(hold_mutex);
        std::vector<Hold> kept_holds;
        for (Hold& hold : holds)
        {
            if (hold.channel != playing)
            {
                kept_holds.push_back(hold);
                continue;
            }
            if (!played)
            {
                world->releaseEntity(hold.entity);
            }
        }
        holds = std::move(kept_holds);
    }
};

CommandBuffer::CommandBuffer(World& world) : impl_(std::make_unique<Impl>())
{
    impl_->world = &world;
    impl_->impl = world.impl_.get();
    impl_->segments.push_back(std::make_unique<Segment>());
}

CommandBuffer::~CommandBuffer()
{
    std::size_t unplayed = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->hold_mutex);
        unplayed = impl_->holds.size();
    }
    if (unplayed != 0U)
    {
        impl_->impl->say(World::Report::Warning,
                         "%zu recorded create(s) were never played; their handles are released",
                         unplayed);
    }
    clear();
}

void CommandBuffer::setChannel(Channel channel) noexcept
{
    impl_->channel = channel;
}

CommandBuffer::Channel CommandBuffer::channel() const noexcept
{
    return impl_->channel;
}

Entity CommandBuffer::create()
{
    return impl_->createInto(impl_->inlineSegment());
}

void CommandBuffer::destroy(Entity entity)
{
    impl_->destroyInto(impl_->inlineSegment(), entity);
}

void CommandBuffer::add(Entity entity, ComponentIndex component, const void* value)
{
    impl_->addInto(impl_->inlineSegment(), entity, component, value);
}

void CommandBuffer::remove(Entity entity, ComponentIndex component)
{
    impl_->removeInto(impl_->inlineSegment(), entity, component);
}

void CommandBuffer::record(PlayFn play, void* context, const void* payload, std::size_t size)
{
    impl_->recordInto(impl_->inlineSegment(), play, context, payload, size);
}

/* --- sections --- */

CommandBuffer::Section::Section(CommandBuffer& buffer, std::uint64_t order_key, Channel channel)
    : buffer_(&buffer)
{
    Impl& source = *buffer.impl_;
    std::lock_guard<std::mutex> lock(source.segment_mutex);
    auto segment = std::make_unique<Segment>();
    segment->is_section = true;
    segment->channel = channel;
    segment->order_key = order_key;
    segment->sequence = source.section_sequence++;
    segment_ = segment.get();
    source.segments.push_back(std::move(segment));
    ++source.open_sections;
}

CommandBuffer::Section::Section(Section&& source) noexcept
    : buffer_(source.buffer_), segment_(source.segment_)
{
    source.buffer_ = nullptr;
}

CommandBuffer::Section::~Section()
{
    if (buffer_ == nullptr)
    {
        return;
    }
    Impl& source = *buffer_->impl_;
    std::lock_guard<std::mutex> lock(source.segment_mutex);
    --source.open_sections;
}

CommandBuffer::Section CommandBuffer::section(std::uint64_t order_key)
{
    return Section(*this, order_key, impl_->channel);
}

Entity CommandBuffer::Section::create()
{
    return buffer_->impl_->createInto(*segment_);
}

void CommandBuffer::Section::destroy(Entity entity)
{
    buffer_->impl_->destroyInto(*segment_, entity);
}

void CommandBuffer::Section::add(Entity entity, ComponentIndex component, const void* value)
{
    buffer_->impl_->addInto(*segment_, entity, component, value);
}

void CommandBuffer::Section::remove(Entity entity, ComponentIndex component)
{
    buffer_->impl_->removeInto(*segment_, entity, component);
}

void CommandBuffer::Section::record(PlayFn play, void* context, const void* payload,
                                    std::size_t size)
{
    buffer_->impl_->recordInto(*segment_, play, context, payload, size);
}

/* --- inspection --- */

World& CommandBuffer::world() noexcept
{
    return *impl_->world;
}

std::uint32_t CommandBuffer::commandCount() const noexcept
{
    std::uint32_t count = 0;
    for (const std::unique_ptr<Segment>& segment : impl_->segments)
    {
        count += static_cast<std::uint32_t>(segment->commands.size());
    }
    return count;
}

std::uint32_t CommandBuffer::commandCount(Channel channel) const noexcept
{
    std::uint32_t count = 0;
    for (const std::unique_ptr<Segment>& segment : impl_->segments)
    {
        if (segment->channel == channel)
        {
            count += static_cast<std::uint32_t>(segment->commands.size());
        }
    }
    return count;
}

bool CommandBuffer::empty() const noexcept
{
    return commandCount() == 0U;
}

bool CommandBuffer::empty(Channel channel) const noexcept
{
    return commandCount(channel) == 0U;
}

void CommandBuffer::clear()
{
    impl_->releaseChannel(Channel::SyncPoint, false);
    impl_->releaseChannel(Channel::NextFrame, false);
    impl_->section_sequence = 0;
    impl_->open_sections = 0;
}

void CommandBuffer::clear(Channel channel)
{
    impl_->releaseChannel(channel, false);
}

/* --- playback --- */

void World::playChannel(CommandBuffer& buffer, CommandBuffer::Channel channel)
{
    if (!buffer.impl_->impl->structural())
    {
        return;
    }

    const std::vector<CommandBuffer::Segment*> order = buffer.impl_->mergeOrder(channel);
    for (CommandBuffer::Segment* segment : order)
    {
        for (const CommandBuffer::Segment::Command& command : segment->commands)
        {
            switch (command.kind)
            {
                case CommandBuffer::Segment::Kind::Create:
                    /* A create that cannot place its entity gives the
                       reservation back rather than leaking it. */
                    if (!buffer.impl_->impl->materialize(command.entity))
                    {
                        buffer.impl_->impl->releaseReservation(command.entity);
                    }
                    break;
                case CommandBuffer::Segment::Kind::Destroy:
                    destroy(command.entity);
                    break;
                case CommandBuffer::Segment::Kind::Add:
                    add(command.entity, command.component,
                        segment->bytes.data() + command.payload);
                    break;
                case CommandBuffer::Segment::Kind::Remove:
                    remove(command.entity, command.component);
                    break;
                case CommandBuffer::Segment::Kind::Recorded:
                    command.play(command.context, *this, segment->bytes.data() + command.payload);
                    break;
            }
        }
    }

    /* Every create of this channel has materialized or given its reservation
       back, so its holds are resolved. The other channel keeps its own. */
    buffer.impl_->releaseChannel(channel, true);
}

void World::play(CommandBuffer& buffer)
{
    playChannel(buffer, CommandBuffer::Channel::SyncPoint);
}

void World::playNextFrame(CommandBuffer& buffer)
{
    playChannel(buffer, CommandBuffer::Channel::NextFrame);
}

} // namespace resonate::ecs
