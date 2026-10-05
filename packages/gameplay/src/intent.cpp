#include <resonate/gameplay/intent.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include <resonate/core/arena.h>

namespace resonate::gameplay
{
namespace
{

/* Non-zero while the calling thread runs inside a wave dispatch: a submission
   from there is consumed within this adjudication, so its entry can come from
   the thread's frame arena. Stage-time submissions outlive the arena's reset
   and take the heap. */
thread_local std::uint32_t t_wave_depth = 0;

/* Scopes the wave-time marker: a submission inside one is consumed by this
   adjudication, so it may take the arena. */
struct WaveDepth
{
    WaveDepth() noexcept
    {
        ++t_wave_depth;
    }
    ~WaveDepth() noexcept
    {
        --t_wave_depth;
    }
    WaveDepth(const WaveDepth&) = delete;
    WaveDepth& operator=(const WaveDepth&) = delete;
};

} // namespace

/* One queued intent. The payload lives behind the entry: one allocation covers
   both, from the heap (stage-time submissions and cap spills, which outlive
   the frame's arena) or from the submitting thread's frame arena (wave-time
   submissions, consumed within the adjudication). */
struct IntentBus::Entry
{
    IntentTypeId type = kInvalidIntent;
    ecs::Entity source{};
    ecs::Entity target{};
    std::uint32_t payload_bytes = 0;
    bool arena_backed = false;

    [[nodiscard]] std::byte* payload() noexcept
    {
        return reinterpret_cast<std::byte*>(this) + sizeof(Entry);
    }
    [[nodiscard]] const std::byte* payload() const noexcept
    {
        return reinterpret_cast<const std::byte*>(this) + sizeof(Entry);
    }
};

struct IntentBus::TypeInfo
{
    const char* name = nullptr;
    std::uint32_t payload_bytes = 0;
    UnhandledPolicy policy = UnhandledPolicy::Ignore;
};

struct IntentBus::HandlerEntry
{
    HandlerDesc desc;
    std::uint32_t sequence = 0; /* registration order; the recording key */
};

struct IntentBus::Impl
{
    ecs::World& world;
    Allocator* allocator = nullptr;
    ecs::CommandBuffer commands;

    std::vector<TypeInfo> types;
    std::vector<HandlerEntry> handlers;

    /* Submit order across types; the waves group them by type in registration
       order, keeping the per-type FIFO. */
    std::vector<Entry*> pending;
    std::vector<Entry*> aftermath;
    mutable std::mutex queue_mutex;

    std::uint32_t depth_cap = 4;
    Stats stats;

    explicit Impl(ecs::World& world_in, Allocator& allocator_in)
        : world(world_in), allocator(&allocator_in), commands(world_in)
    {
    }

    ~Impl()
    {
        /* Heap entries are the bus's; arena entries belong to the frame
           arenas, which reset without asking. */
        std::vector<Entry*> leftover;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            leftover = std::move(pending);
            leftover.insert(leftover.end(), aftermath.begin(), aftermath.end());
            pending.clear();
            aftermath.clear();
        }
        if (!leftover.empty())
        {
            world.report(ecs::World::Report::Warning,
                         "%zu intent(s) were never adjudicated; released", leftover.size());
            for (Entry* entry : leftover)
            {
                freeEntry(entry);
            }
        }
    }

    void freeEntry(Entry* entry)
    {
        if (entry != nullptr && !entry->arena_backed)
        {
            allocator->deallocate(entry, sizeof(Entry) + entry->payload_bytes);
        }
    }

    /* An arena-backed entry cannot outlive the frame's reset, so a spill
       copies it onto the heap before the aftermath holds it. */
    Entry* toHeap(Entry* entry)
    {
        if (entry == nullptr || !entry->arena_backed)
        {
            return entry;
        }

        auto* copy = static_cast<Entry*>(allocator->allocate(sizeof(Entry) + entry->payload_bytes,
                                                            alignof(Entry)));
        copy->type = entry->type;
        copy->source = entry->source;
        copy->target = entry->target;
        copy->payload_bytes = entry->payload_bytes;
        copy->arena_backed = false;
        if (entry->payload_bytes != 0U)
        {
            std::memcpy(reinterpret_cast<std::byte*>(copy) + sizeof(Entry), entry->payload(),
                        entry->payload_bytes);
        }
        return copy;
    }

    /* --- the wave dispatch --- */

    struct WaveContext
    {
        Impl* bus = nullptr;
        HandlerEntry* handler = nullptr;
        const std::vector<const Entry*>* entries = nullptr;
        std::uint32_t dropped = 0;
    };

    static void waveTask(void* context)
    {
        auto* task = static_cast<WaveContext*>(context);
        WaveDepth depth;

        /* The handler's private segment of the bus's buffer, merged by the
           handler's registration order at the next sync point. */
        ecs::CommandBuffer::Section section =
            task->bus->commands.section(task->handler->sequence);

        for (const Entry* entry : *task->entries)
        {
            if (!task->bus->world.alive(entry->target))
            {
                /* The endpoint died between submit and dispatch; there is no
                   owner left to adjudicate. */
                task->bus->world.report(
                    ecs::World::Report::Warning,
                    "intents: intent of type '%s' names target %u:%u, which is not alive; dropped",
                    task->bus->types[entry->type].name, entry->target.index,
                    entry->target.generation);
                ++task->dropped;
                continue;
            }

            const Intent intent{entry->type, entry->source, entry->target, entry->payload()};
            task->handler->desc.run(task->handler->desc.user, task->bus->world, section, intent);
        }
    }

    /* One adjudication: waves until the queue is quiet or the cap, then the
       rest spills to the aftermath. The parallel scope is what makes a
       handler's direct structural call refused — recording is the way. */
    void adjudicateWaves(JobSystem* jobs, bool with_aftermath)
    {
        std::uint32_t settled = 0;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (with_aftermath && !aftermath.empty())
            {
                settled = static_cast<std::uint32_t>(aftermath.size());
                pending.insert(pending.end(), aftermath.begin(), aftermath.end());
                aftermath.clear();
            }
        }

        stats = Stats{};
        stats.settled = settled;
        ecs::World::ParallelScope scope(world);

        for (std::uint32_t wave = 0;; ++wave)
        {
            std::vector<Entry*> batch;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                batch.swap(pending);
            }
            if (batch.empty())
            {
                break;
            }
            if (wave >= depth_cap)
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                for (Entry* entry : batch)
                {
                    aftermath.push_back(toHeap(entry));
                }
                stats.spilled += static_cast<std::uint32_t>(batch.size());
                break;
            }
            ++stats.waves;

            /* Grouped by type in registration order; the per-type order stays
               the submission order, FIFO. */
            std::vector<std::vector<const Entry*>> byType(types.size());
            for (const Entry* entry : batch)
            {
                byType[entry->type].push_back(entry);
            }

            std::uint32_t job_count = 0;
            for (std::uint32_t type = 0; type < types.size(); ++type)
            {
                if (byType[type].empty())
                {
                    continue;
                }

                std::uint32_t type_handlers = 0;
                for (const HandlerEntry& handler : handlers)
                {
                    if (handler.desc.type == type)
                    {
                        ++type_handlers;
                    }
                }
                if (type_handlers == 0U)
                {
                    stats.dropped += static_cast<std::uint32_t>(byType[type].size());
                    if (types[type].policy == UnhandledPolicy::Reject)
                    {
                        world.report(ecs::World::Report::Warning,
                                     "intents: %u intent(s) of type '%s' have no handler; "
                                     "dropped",
                                     static_cast<unsigned>(byType[type].size()),
                                     types[type].name);
                    }
                    continue;
                }
                job_count += type_handlers;
            }

            /* The contexts are the job bodies' storage: reserved up front, so
               the pointers a submission hands the pool stay put. */
            std::vector<WaveContext> contexts(job_count);
            std::vector<JobIndex> handles;
            handles.reserve(job_count);

            std::size_t context_index = 0;
            for (std::uint32_t type = 0; type < types.size(); ++type)
            {
                if (byType[type].empty())
                {
                    continue;
                }

                bool handled = false;
                for (HandlerEntry& handler : handlers)
                {
                    if (handler.desc.type != type)
                    {
                        continue;
                    }
                    handled = true;

                    WaveContext& context = contexts[context_index++];
                    context.bus = this;
                    context.handler = &handler;
                    context.entries = &byType[type];
                    if (jobs != nullptr)
                    {
                        handles.push_back(jobs->submitSingle(
                            &Impl::waveTask, &context, handler.desc.reads, handler.desc.writes,
                            JobPriorityHigh, JobAffinityLatency));
                    }
                    else
                    {
                        waveTask(&context);
                    }
                }
                if (handled)
                {
                    stats.dispatched += static_cast<std::uint32_t>(byType[type].size());
                }
            }

            for (const JobIndex handle : handles)
            {
                jobs->wait(handle);
            }
            for (const WaveContext& context : contexts)
            {
                stats.dropped += context.dropped;
            }

            /* The batch is consumed: the handlers ran, the refusals are
               counted. Heap entries are the bus's to free; arena entries die
               with the frame's reset. */
            for (Entry* entry : batch)
            {
                freeEntry(entry);
            }
        }
    }
};

IntentBus::IntentBus(ecs::World& world, Allocator& allocator)
    : impl_(std::make_unique<Impl>(world, allocator))
{
}

IntentBus::~IntentBus() = default;

IntentTypeId IntentBus::registerType(const char* name, std::uint32_t payload_bytes,
                                     UnhandledPolicy unhandled)
{
    if (name == nullptr || *name == '\0')
    {
        impl_->world.report(ecs::World::Report::Error, "intents: a type must be named");
        return kInvalidIntent;
    }

    for (std::uint32_t index = 0; index < impl_->types.size(); ++index)
    {
        const TypeInfo& type = impl_->types[index];
        if (std::strcmp(type.name, name) != 0)
        {
            continue;
        }
        if (type.payload_bytes != payload_bytes)
        {
            impl_->world.report(ecs::World::Report::Error,
                                "intents: type '%s' is already registered with %u payload "
                                "byte(s), not %u",
                                name, type.payload_bytes, payload_bytes);
            return kInvalidIntent;
        }
        return index;
    }

    impl_->types.push_back(TypeInfo{name, payload_bytes, unhandled});
    return static_cast<IntentTypeId>(impl_->types.size() - 1U);
}

bool IntentBus::addHandler(const HandlerDesc& desc)
{
    if (desc.type == kInvalidIntent || desc.type >= impl_->types.size())
    {
        impl_->world.report(ecs::World::Report::Error,
                            "intents: a handler needs a registered type");
        return false;
    }
    if (desc.run == nullptr)
    {
        impl_->world.report(ecs::World::Report::Error,
                            "intents: handler '%s' has no run function",
                            desc.name != nullptr ? desc.name : "");
        return false;
    }

    impl_->handlers.push_back(HandlerEntry{desc, static_cast<std::uint32_t>(
                                                     impl_->handlers.size())});
    return true;
}

bool IntentBus::submit(IntentTypeId type, ecs::Entity source, ecs::Entity target,
                       const void* payload)
{
    if (type == kInvalidIntent || type >= impl_->types.size())
    {
        impl_->world.report(ecs::World::Report::Error, "intents: submit of an unregistered type");
        return false;
    }
    const TypeInfo& info = impl_->types[type];

    /* The endpoints are the submitter's claim about live state; a stale one is
       a caller bug worth naming now, not at dispatch. The target is checked
       again at dispatch, where a death in between is a dropped intent. */
    if (!target.valid() || !impl_->world.alive(target))
    {
        impl_->world.report(ecs::World::Report::Error,
                            "intents: submit of '%s' names target %u:%u, which is not alive",
                            info.name, target.index, target.generation);
        return false;
    }
    if (source.valid() && !impl_->world.alive(source))
    {
        impl_->world.report(ecs::World::Report::Error,
                            "intents: submit of '%s' names source %u:%u, which is not alive",
                            info.name, source.index, source.generation);
        return false;
    }

    Entry* entry = nullptr;
    const std::uint32_t bytes = info.payload_bytes;
    if (t_wave_depth != 0U)
    {
        if (FrameArenas* arenas = FrameArenas::active(); arenas != nullptr)
        {
            void* memory = arenas->current().allocate(sizeof(Entry) + bytes, alignof(Entry));
            if (memory != nullptr)
            {
                entry = static_cast<Entry*>(memory);
                entry->arena_backed = true;
            }
        }
    }
    if (entry == nullptr)
    {
        entry = static_cast<Entry*>(impl_->allocator->allocate(sizeof(Entry) + bytes,
                                                               alignof(Entry)));
        if (entry == nullptr)
        {
            impl_->world.report(ecs::World::Report::Error,
                                "intents: entry allocation of %zu byte(s) failed",
                                sizeof(Entry) + bytes);
            return false;
        }
        entry->arena_backed = false;
    }

    entry->type = type;
    entry->source = source;
    entry->target = target;
    entry->payload_bytes = bytes;
    if (bytes != 0U)
    {
        if (payload != nullptr)
        {
            std::memcpy(entry->payload(), payload, bytes);
        }
        else
        {
            std::memset(entry->payload(), 0, bytes);
        }
    }

    std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    impl_->pending.push_back(entry);
    return true;
}

void IntentBus::adjudicate(JobSystem* jobs)
{
    impl_->adjudicateWaves(jobs, false);
}

void IntentBus::settleAftermath(JobSystem* jobs)
{
    impl_->adjudicateWaves(jobs, true);
}

void IntentBus::setDepthCap(std::uint32_t waves) noexcept
{
    impl_->depth_cap = waves;
}

std::uint32_t IntentBus::depthCap() const noexcept
{
    return impl_->depth_cap;
}

std::uint32_t IntentBus::pendingCount() const
{
    std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    return static_cast<std::uint32_t>(impl_->pending.size());
}

ecs::CommandBuffer& IntentBus::commands() noexcept
{
    return impl_->commands;
}

const IntentBus::Stats& IntentBus::lastStats() const noexcept
{
    return impl_->stats;
}

} // namespace resonate::gameplay
