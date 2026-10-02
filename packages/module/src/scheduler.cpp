#include <resonate/module/stage.h>

#include <cstdio>
#include <cstring>

#include <resonate/core/job.h>

#include "host_alloc.h"

namespace
{

constexpr uint32_t INITIAL_CAPACITY = 16;

/* Every group: what a system that declares no group, or that asks for an
   exclusive stage, is serialised through. Parallel execution is opt-in, so a
   declaration of nothing cannot be read as a claim of independence. */
constexpr ResonateResourceGroup ALL_GROUPS = 0xFFFFFFFFU;

/* The scheduler owns a copy of the descriptor's name; a temporary string is fine. */
struct System
{
    ResonateSystemDesc desc;
    char* name;
};

/* A system of the stage being run: the job body takes one pointer, so the run
   function and the frame's delta seconds ride together here. The scratch array
   stays alive until the stage's waits have returned. */
struct ScheduledRun
{
    ResonateSystemFn run;
    void* context;
    float delta_seconds;
    resonate::JobIndex handle;
};

void runScheduled(void* context)
{
    auto* scheduled = static_cast<ScheduledRun*>(context);
    scheduled->run(scheduled->context, scheduled->delta_seconds);
}

} // namespace

struct ResonateScheduler
{
    const ResonateHostApi* host;
    System* systems;
    uint32_t count;
    uint32_t capacity;

    ScheduledRun* runs;

    /* The execution substrate the stage graph runs on, owned here because the
       schedule is what drives it. Null when it could not be created, which
       degrades a stage to registration order instead of failing. */
    resonate::JobSystem* jobs;
};

namespace
{

bool grow(ResonateScheduler* scheduler)
{
    const uint32_t next_capacity = scheduler->capacity * 2U;
    void* memory = resonate::detail::hostAllocate(scheduler->host, sizeof(System) * next_capacity,
                                                  alignof(System));
    void* runs_memory = resonate::detail::hostAllocate(
        scheduler->host, sizeof(ScheduledRun) * next_capacity, alignof(ScheduledRun));
    if (memory == nullptr || runs_memory == nullptr)
    {
        resonate::detail::hostDeallocate(scheduler->host, memory, sizeof(System) * next_capacity);
        resonate::detail::hostDeallocate(scheduler->host, runs_memory,
                                         sizeof(ScheduledRun) * next_capacity);
        return false;
    }

    auto* systems = static_cast<System*>(memory);
    for (uint32_t index = 0; index < scheduler->count; ++index)
    {
        systems[index] = scheduler->systems[index];
    }

    resonate::detail::hostDeallocate(scheduler->host, scheduler->systems,
                                     sizeof(System) * scheduler->capacity);
    resonate::detail::hostDeallocate(scheduler->host, scheduler->runs,
                                     sizeof(ScheduledRun) * scheduler->capacity);
    scheduler->systems = systems;
    /* Scratch: only the capacity matters, the contents are written per stage. */
    scheduler->runs = static_cast<ScheduledRun*>(runs_memory);
    scheduler->capacity = next_capacity;
    return true;
}

char* copyName(const ResonateHostApi* host, const char* name)
{
    const char* source = name != nullptr ? name : "";
    const size_t size = std::strlen(source) + 1U;

    auto* copy = static_cast<char*>(resonate::detail::hostAllocate(host, size, 1U));
    if (copy != nullptr)
    {
        std::memcpy(copy, source, size);
    }
    return copy;
}

bool conflicts(const ResonateSystemDesc& a, const ResonateSystemDesc& b)
{
    const ResonateResourceGroup wrote = b.writes;
    return (a.reads & wrote) != 0U || (a.writes & b.writes) != 0U || (a.writes & b.reads) != 0U;
}

ResonateResourceGroup conflictMask(const ResonateSystemDesc& a, const ResonateSystemDesc& b)
{
    return static_cast<ResonateResourceGroup>((a.reads & b.writes) | (a.writes & b.writes) |
                                              (a.writes & b.reads));
}

void reportConflicts(ResonateScheduler* scheduler, const System& added)
{
    for (uint32_t index = 0; index < scheduler->count; ++index)
    {
        const System& other = scheduler->systems[index];
        if (other.desc.stage != added.desc.stage || !conflicts(other.desc, added.desc))
        {
            continue;
        }

        char message[256] = {};
        std::snprintf(
            message, sizeof(message),
            "systems '%s' and '%s' both touch resource group 0x%x in stage %u; serialised",
            other.name, added.name, conflictMask(other.desc, added.desc),
            static_cast<unsigned>(added.desc.stage));
        resonate::detail::hostLog(scheduler->host, RESONATE_LOG_WARN, message);
    }
}

/* Index of the first system matching the key, or count when none does. */
uint32_t indexOf(const ResonateScheduler* scheduler, const char* name, ResonateSystemFn run,
                 void* context, bool exact)
{
    for (uint32_t index = 0; index < scheduler->count; ++index)
    {
        const System& system = scheduler->systems[index];
        if (std::strcmp(system.name, name) != 0)
        {
            continue;
        }
        if (!exact || (system.desc.run == run && system.desc.context == context))
        {
            return index;
        }
    }
    return scheduler->count;
}

void removeAt(ResonateScheduler* scheduler, uint32_t index)
{
    System& system = scheduler->systems[index];
    resonate::detail::hostDeallocate(scheduler->host, system.name,
                                     std::strlen(system.name) + 1U);
    for (uint32_t move = index; move + 1U < scheduler->count; ++move)
    {
        scheduler->systems[move] = scheduler->systems[move + 1U];
    }
    --scheduler->count;
}

} // namespace

extern "C"
{

ResonateStatus resonate_scheduler_create(ResonateScheduler** out_scheduler,
                                         const ResonateHostApi* host)
{
    if (out_scheduler == nullptr || host == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    *out_scheduler = nullptr;

    void* memory =
        resonate::detail::hostAllocate(host, sizeof(ResonateScheduler), alignof(ResonateScheduler));
    if (memory == nullptr)
    {
        return RESONATE_E_INTERNAL;
    }

    auto* scheduler = static_cast<ResonateScheduler*>(memory);
    *scheduler = ResonateScheduler{host, nullptr, 0, INITIAL_CAPACITY, nullptr, nullptr};

    void* systems =
        resonate::detail::hostAllocate(host, sizeof(System) * INITIAL_CAPACITY, alignof(System));
    void* runs = resonate::detail::hostAllocate(host, sizeof(ScheduledRun) * INITIAL_CAPACITY,
                                                alignof(ScheduledRun));
    if (systems == nullptr || runs == nullptr)
    {
        resonate::detail::hostDeallocate(host, systems, sizeof(System) * INITIAL_CAPACITY);
        resonate::detail::hostDeallocate(host, runs, sizeof(ScheduledRun) * INITIAL_CAPACITY);
        resonate::detail::hostDeallocate(host, scheduler, sizeof(ResonateScheduler));
        return RESONATE_E_INTERNAL;
    }
    scheduler->systems = static_cast<System*>(systems);
    scheduler->runs = static_cast<ScheduledRun*>(runs);

    scheduler->jobs = resonate::createJobSystem();

    *out_scheduler = scheduler;
    return RESONATE_OK;
}

void resonate_scheduler_destroy(ResonateScheduler* scheduler)
{
    if (scheduler == nullptr)
    {
        return;
    }

    const ResonateHostApi* host = scheduler->host;

    /* The pool drains what it holds before it goes, and a system still running
       reads its entry in the scratch array — so it is deleted before any of the
       scheduler's storage is freed. */
    delete scheduler->jobs;

    for (uint32_t index = 0; index < scheduler->count; ++index)
    {
        resonate::detail::hostDeallocate(host, scheduler->systems[index].name,
                                         std::strlen(scheduler->systems[index].name) + 1U);
    }
    resonate::detail::hostDeallocate(host, scheduler->systems,
                                     sizeof(System) * scheduler->capacity);
    resonate::detail::hostDeallocate(host, scheduler->runs,
                                     sizeof(ScheduledRun) * scheduler->capacity);
    resonate::detail::hostDeallocate(host, scheduler, sizeof(ResonateScheduler));
}

ResonateStatus resonate_scheduler_add_system(ResonateScheduler* scheduler,
                                             const ResonateSystemDesc* desc)
{
    if (scheduler == nullptr || desc == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    if (desc->run == nullptr || desc->stage >= RESONATE_STAGE_COUNT)
    {
        return RESONATE_E_INVALID;
    }
    if (desc->struct_size != 0U && desc->struct_size < sizeof(ResonateSystemDesc))
    {
        return RESONATE_E_VERSION;
    }

    if (scheduler->count == scheduler->capacity && !grow(scheduler))
    {
        return RESONATE_E_INTERNAL;
    }

    char* name = copyName(scheduler->host, desc->name);
    if (name == nullptr)
    {
        return RESONATE_E_INTERNAL;
    }

    System& added = scheduler->systems[scheduler->count];
    added.desc = *desc;
    added.name = name;
    added.desc.name = name;

    reportConflicts(scheduler, added);
    ++scheduler->count;
    return RESONATE_OK;
}

void resonate_scheduler_remove_system(ResonateScheduler* scheduler, const char* name)
{
    if (scheduler == nullptr || name == nullptr)
    {
        return;
    }

    const uint32_t index = indexOf(scheduler, name, nullptr, nullptr, false);
    if (index < scheduler->count)
    {
        removeAt(scheduler, index);
    }
}

void resonate_scheduler_remove_system_exact(ResonateScheduler* scheduler, const char* name,
                                            ResonateSystemFn run, void* context)
{
    if (scheduler == nullptr || name == nullptr)
    {
        return;
    }

    const uint32_t index = indexOf(scheduler, name, run, context, true);
    if (index < scheduler->count)
    {
        removeAt(scheduler, index);
    }
}

void resonate_scheduler_run_stage(ResonateScheduler* scheduler, ResonateStage stage,
                                  float delta_seconds)
{
    if (scheduler == nullptr || stage >= RESONATE_STAGE_COUNT)
    {
        return;
    }

    /* Registration order is the submission order, and the executor serialises
       conflicting systems in it: a system never starts before one it shares a
       group with has completed. Systems that share no group are free to run at
       the same time. */
    uint32_t scheduled = 0;
    for (uint32_t index = 0; index < scheduler->count; ++index)
    {
        const System& system = scheduler->systems[index];
        if (system.desc.stage != stage)
        {
            continue;
        }

        ScheduledRun& run = scheduler->runs[scheduled];
        run.run = system.desc.run;
        run.context = system.desc.context;
        run.delta_seconds = delta_seconds;

        if (scheduler->jobs == nullptr)
        {
            runScheduled(&run);
            continue;
        }

        /* A system that declares no group claims nothing to share, and an
           exclusive one must not share the stage with anything: both are ordered
           against every other system rather than assumed independent. */
        const bool ordered =
            (system.desc.reads | system.desc.writes) == 0U || system.desc.exclusive_stage != 0;
        const ResonateResourceGroup writes = ordered ? ALL_GROUPS : system.desc.writes;

        run.handle =
            scheduler->jobs->submitSingle(&runScheduled, &run, system.desc.reads, writes,
                                          resonate::JobPriorityHigh, resonate::JobAffinityLatency);
        ++scheduled;
    }

    if (scheduler->jobs == nullptr)
    {
        return;
    }

    /* The stage is a barrier: it returns once every system it submitted has
       completed. Waiting on the handles rather than on everything keeps work
       submitted elsewhere — a background load, say — out of the frame; each wait
       participates, so the calling thread runs queued work instead of idling. */
    for (uint32_t index = 0; index < scheduled; ++index)
    {
        scheduler->jobs->wait(scheduler->runs[index].handle);
    }
}

void resonate_scheduler_run_frame(ResonateScheduler* scheduler, float delta_seconds)
{
    for (uint8_t stage = RESONATE_STAGE_EARLY_UPDATE; stage < RESONATE_STAGE_COUNT; ++stage)
    {
        resonate_scheduler_run_stage(scheduler, static_cast<ResonateStage>(stage), delta_seconds);
    }
}

} // extern "C"

namespace resonate
{

JobSystem* jobsOf(ResonateScheduler* scheduler)
{
    return scheduler != nullptr ? scheduler->jobs : nullptr;
}

} // namespace resonate
