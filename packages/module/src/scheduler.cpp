#include <resonate/module/stage.h>

#include <cstdio>
#include <cstring>

#include "host_alloc.h"

namespace
{

constexpr uint32_t INITIAL_CAPACITY = 16;

/* The scheduler owns a copy of the descriptor's name; a temporary string is fine. */
struct System
{
    ResonateSystemDesc desc;
    char* name;
};

} // namespace

struct ResonateScheduler
{
    const ResonateHostApi* host;
    System* systems;
    uint32_t count;
    uint32_t capacity;
};

namespace
{

bool grow(ResonateScheduler* scheduler)
{
    const uint32_t next_capacity = scheduler->capacity * 2U;
    void* memory = resonate::detail::hostAllocate(scheduler->host, sizeof(System) * next_capacity,
                                                  alignof(System));
    if (memory == nullptr)
    {
        return false;
    }

    auto* systems = static_cast<System*>(memory);
    for (uint32_t index = 0; index < scheduler->count; ++index)
    {
        systems[index] = scheduler->systems[index];
    }

    resonate::detail::hostDeallocate(scheduler->host, scheduler->systems,
                                     sizeof(System) * scheduler->capacity);
    scheduler->systems = systems;
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
    *scheduler = ResonateScheduler{host, nullptr, 0, INITIAL_CAPACITY};

    void* systems =
        resonate::detail::hostAllocate(host, sizeof(System) * INITIAL_CAPACITY, alignof(System));
    if (systems == nullptr)
    {
        resonate::detail::hostDeallocate(host, scheduler, sizeof(ResonateScheduler));
        return RESONATE_E_INTERNAL;
    }
    scheduler->systems = static_cast<System*>(systems);

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
    for (uint32_t index = 0; index < scheduler->count; ++index)
    {
        resonate::detail::hostDeallocate(host, scheduler->systems[index].name,
                                         std::strlen(scheduler->systems[index].name) + 1U);
    }
    resonate::detail::hostDeallocate(host, scheduler->systems,
                                     sizeof(System) * scheduler->capacity);
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

    for (uint32_t index = 0; index < scheduler->count; ++index)
    {
        const System& system = scheduler->systems[index];
        if (system.desc.stage == stage)
        {
            system.desc.run(system.desc.context, delta_seconds);
        }
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
