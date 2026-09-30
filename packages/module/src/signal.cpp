#include <resonate/module/signal.h>

#include "host_alloc.h"

namespace
{

constexpr uint32_t INITIAL_CAPACITY = 8;

/* An entry with a null node is a tombstone: shifting instead would move entries
   the running emit is walking over. Cleared when no emit is in progress. */
struct Subscriber
{
    ResonateSignalNode* node;
    ResonateSignalInvoke invoke;
};

} // namespace

struct ResonateSignalStorage
{
    const ResonateHostApi* host;
    Subscriber* entries;
    uint32_t count;
    uint32_t capacity;
    uint32_t emit_depth;
};

namespace
{

bool grow(ResonateSignalStorage* storage)
{
    const uint32_t next_capacity = storage->capacity * 2U;
    void* memory = resonate::detail::hostAllocate(storage->host, sizeof(Subscriber) * next_capacity,
                                                  alignof(Subscriber));
    if (memory == nullptr)
    {
        return false;
    }

    auto* entries = static_cast<Subscriber*>(memory);
    for (uint32_t index = 0; index < storage->count; ++index)
    {
        entries[index] = storage->entries[index];
    }

    resonate::detail::hostDeallocate(storage->host, storage->entries,
                                     sizeof(Subscriber) * storage->capacity);
    storage->entries = entries;
    storage->capacity = next_capacity;
    return true;
}

void compact(ResonateSignalStorage* storage)
{
    uint32_t kept = 0;
    for (uint32_t index = 0; index < storage->count; ++index)
    {
        if (storage->entries[index].node != nullptr)
        {
            storage->entries[kept++] = storage->entries[index];
        }
    }
    storage->count = kept;
}

/* limit is frozen at entry, so a subscriber that connects during the emit is not
   visited until the next one. */
uint32_t walk(ResonateSignalStorage* storage, const void* payload, bool stop_on_nonzero, bool* ran)
{
    const uint32_t limit = storage->count;
    uint32_t result = 0;
    *ran = false;

    ++storage->emit_depth;
    for (uint32_t index = 0; index < limit; ++index)
    {
        const Subscriber entry = storage->entries[index];
        if (entry.node == nullptr)
        {
            continue;
        }
        *ran = true;
        const uint32_t subscriber_result = entry.invoke(entry.node->subscriber, payload);
        if (subscriber_result != 0U)
        {
            result = subscriber_result;
            if (stop_on_nonzero)
            {
                break;
            }
        }
    }
    --storage->emit_depth;

    if (storage->emit_depth == 0)
    {
        compact(storage);
    }
    return result;
}

} // namespace

extern "C"
{

ResonateStatus resonate_signal_create(ResonateSignalStorage** out_storage,
                                      const ResonateHostApi* host)
{
    if (out_storage == nullptr || host == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    *out_storage = nullptr;

    void* memory = resonate::detail::hostAllocate(host, sizeof(ResonateSignalStorage),
                                                  alignof(ResonateSignalStorage));
    if (memory == nullptr)
    {
        return RESONATE_E_INTERNAL;
    }

    auto* storage = static_cast<ResonateSignalStorage*>(memory);
    *storage = ResonateSignalStorage{host, nullptr, 0, INITIAL_CAPACITY, 0};

    void* entries = resonate::detail::hostAllocate(host, sizeof(Subscriber) * INITIAL_CAPACITY,
                                                   alignof(Subscriber));
    if (entries == nullptr)
    {
        resonate::detail::hostDeallocate(host, storage, sizeof(ResonateSignalStorage));
        return RESONATE_E_INTERNAL;
    }
    storage->entries = static_cast<Subscriber*>(entries);

    *out_storage = storage;
    return RESONATE_OK;
}

void resonate_signal_destroy(ResonateSignalStorage* storage)
{
    if (storage == nullptr)
    {
        return;
    }

    /* Every connected node is told the connection is gone before the storage is
       freed, so a node that tears down after its signal — a member destroyed
       before the one that owns the signal — finds nothing to call into rather
       than a dangling storage. */
    for (uint32_t index = 0; index < storage->count; ++index)
    {
        if (storage->entries[index].node != nullptr)
        {
            storage->entries[index].node->storage = nullptr;
        }
    }

    const ResonateHostApi* host = storage->host;
    resonate::detail::hostDeallocate(host, storage->entries,
                                     sizeof(Subscriber) * storage->capacity);
    resonate::detail::hostDeallocate(host, storage, sizeof(ResonateSignalStorage));
}

ResonateStatus resonate_signal_connect(ResonateSignalStorage* storage, ResonateSignalNode* node,
                                       ResonateSignalInvoke invoke)
{
    if (storage == nullptr || node == nullptr || invoke == nullptr)
    {
        return RESONATE_E_INVALID;
    }

    for (uint32_t index = 0; index < storage->count; ++index)
    {
        if (storage->entries[index].node == node)
        {
            return RESONATE_E_INVALID;
        }
    }

    if (storage->count == storage->capacity && !grow(storage))
    {
        return RESONATE_E_INTERNAL;
    }

    storage->entries[storage->count].node = node;
    storage->entries[storage->count].invoke = invoke;
    ++storage->count;
    node->storage = storage;
    return RESONATE_OK;
}

void resonate_signal_disconnect(ResonateSignalStorage* storage, ResonateSignalNode* node)
{
    if (storage == nullptr || node == nullptr)
    {
        return;
    }

    for (uint32_t index = 0; index < storage->count; ++index)
    {
        if (storage->entries[index].node == node)
        {
            storage->entries[index].node = nullptr;
            node->storage = nullptr;
            if (storage->emit_depth == 0)
            {
                compact(storage);
            }
            return;
        }
    }
}

uint32_t resonate_signal_emit(ResonateSignalStorage* storage, const void* payload)
{
    if (storage == nullptr)
    {
        return 0;
    }
    bool ran = false;
    walk(storage, payload, false, &ran);
    return ran ? 1U : 0U;
}

uint32_t resonate_signal_emit_until(ResonateSignalStorage* storage, const void* payload)
{
    if (storage == nullptr)
    {
        return 0;
    }
    bool ran = false;
    return walk(storage, payload, true, &ran);
}

uint32_t resonate_signal_subscriber_count(const ResonateSignalStorage* storage)
{
    if (storage == nullptr)
    {
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t index = 0; index < storage->count; ++index)
    {
        if (storage->entries[index].node != nullptr)
        {
            ++count;
        }
    }
    return count;
}

} // extern "C"
