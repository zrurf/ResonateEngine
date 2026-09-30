#include <resonate/module/capability.h>

#include <cstdio>

#include "host_alloc.h"

struct ResonateCapabilityRegistry
{
    const ResonateHostApi* host;
    ResonateCapabilityRecord* records;
    uint32_t count;
    uint32_t capacity;
};

namespace
{

constexpr uint32_t INITIAL_CAPACITY = 8;

bool sameId(ResonateId a, ResonateId b)
{
    return a.lo == b.lo && a.hi == b.hi;
}

void formatId(ResonateId id, char* buffer, size_t size)
{
    std::snprintf(buffer, size, "%016llx%016llx", static_cast<unsigned long long>(id.hi),
                  static_cast<unsigned long long>(id.lo));
}

const char* describe(const ResonateCapabilityRecord* record, char* buffer, size_t size)
{
    if (record->name != nullptr)
    {
        return record->name;
    }
    formatId(record->id, buffer, size);
    return buffer;
}

bool grow(ResonateCapabilityRegistry* registry)
{
    const uint32_t next_capacity = registry->capacity * 2U;
    void* memory = resonate::detail::hostAllocate(registry->host,
                                                  sizeof(ResonateCapabilityRecord) * next_capacity,
                                                  alignof(ResonateCapabilityRecord));
    if (memory == nullptr)
    {
        return false;
    }

    auto* records = static_cast<ResonateCapabilityRecord*>(memory);
    for (uint32_t index = 0; index < registry->count; ++index)
    {
        records[index] = registry->records[index];
    }

    resonate::detail::hostDeallocate(registry->host, registry->records,
                                     sizeof(ResonateCapabilityRecord) * registry->capacity);
    registry->records = records;
    registry->capacity = next_capacity;
    return true;
}

ResonateCapabilityRecord* find(ResonateCapabilityRegistry* registry, ResonateId id)
{
    for (uint32_t index = 0; index < registry->count; ++index)
    {
        if (sameId(registry->records[index].id, id))
        {
            return &registry->records[index];
        }
    }
    return nullptr;
}

#ifdef RESONATE_DEBUG
ResonateStatus verifyInstance(const ResonateHostApi* host, const ResonateCapabilityRecord* record)
{
    char id_text[40] = {};
    const char* label = describe(record, id_text, sizeof(id_text));

    if (record->instance == nullptr)
    {
        return RESONATE_E_INVALID;
    }

    const auto* header = static_cast<const ResonateCapabilityHeader*>(record->instance);
    if (header->struct_size < sizeof(ResonateCapabilityHeader))
    {
        char message[192] = {};
        std::snprintf(message, sizeof(message),
                      "capability %s reports struct_size %u, below the header", label,
                      header->struct_size);
        resonate::detail::hostLog(host, RESONATE_LOG_ERROR, message);
        return RESONATE_E_INVALID;
    }
    if (record->struct_size != header->struct_size)
    {
        char message[192] = {};
        std::snprintf(message, sizeof(message),
                      "capability %s is registered as %u bytes but its header says %u", label,
                      record->struct_size, header->struct_size);
        resonate::detail::hostLog(host, RESONATE_LOG_ERROR, message);
        return RESONATE_E_INVALID;
    }
    if ((header->thread_mask & ~static_cast<uint32_t>(RESONATE_THREAD_ANY)) != 0U)
    {
        char message[192] = {};
        std::snprintf(message, sizeof(message), "capability %s declares unknown thread bits 0x%x",
                      label, header->thread_mask);
        resonate::detail::hostLog(host, RESONATE_LOG_ERROR, message);
        return RESONATE_E_INVALID;
    }
    return RESONATE_OK;
}
#endif

} // namespace

extern "C"
{

ResonateStatus resonate_capability_registry_create(ResonateCapabilityRegistry** out_registry,
                                                   const ResonateHostApi* host)
{
    if (out_registry == nullptr || host == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    *out_registry = nullptr;

    void* memory = resonate::detail::hostAllocate(host, sizeof(ResonateCapabilityRegistry),
                                                  alignof(ResonateCapabilityRegistry));
    if (memory == nullptr)
    {
        return RESONATE_E_INTERNAL;
    }

    auto* registry = static_cast<ResonateCapabilityRegistry*>(memory);
    *registry = ResonateCapabilityRegistry{host, nullptr, 0, INITIAL_CAPACITY};

    void* records =
        resonate::detail::hostAllocate(host, sizeof(ResonateCapabilityRecord) * INITIAL_CAPACITY,
                                       alignof(ResonateCapabilityRecord));
    if (records == nullptr)
    {
        resonate::detail::hostDeallocate(host, registry, sizeof(ResonateCapabilityRegistry));
        return RESONATE_E_INTERNAL;
    }
    registry->records = static_cast<ResonateCapabilityRecord*>(records);

    *out_registry = registry;
    return RESONATE_OK;
}

void resonate_capability_registry_destroy(ResonateCapabilityRegistry* registry)
{
    if (registry == nullptr)
    {
        return;
    }
    const ResonateHostApi* host = registry->host;
    resonate::detail::hostDeallocate(host, registry->records,
                                     sizeof(ResonateCapabilityRecord) * registry->capacity);
    resonate::detail::hostDeallocate(host, registry, sizeof(ResonateCapabilityRegistry));
}

ResonateStatus resonate_capability_register(ResonateCapabilityRegistry* registry,
                                            const ResonateCapabilityRecord* record)
{
    if (registry == nullptr || record == nullptr)
    {
        return RESONATE_E_INVALID;
    }

    if (find(registry, record->id) != nullptr)
    {
        char id_text[40] = {};
        char message[192] = {};
        std::snprintf(message, sizeof(message), "capability %s is already registered",
                      describe(record, id_text, sizeof(id_text)));
        resonate::detail::hostLog(registry->host, RESONATE_LOG_ERROR, message);
        return RESONATE_E_INVALID;
    }

#ifdef RESONATE_DEBUG
    const ResonateStatus verified = verifyInstance(registry->host, record);
    if (verified != RESONATE_OK)
    {
        return verified;
    }
#endif

    if (registry->count == registry->capacity && !grow(registry))
    {
        return RESONATE_E_INTERNAL;
    }

    registry->records[registry->count++] = *record;
    return RESONATE_OK;
}

void resonate_capability_unregister(ResonateCapabilityRegistry* registry, ResonateId id,
                                    void* instance)
{
    if (registry == nullptr)
    {
        return;
    }

    ResonateCapabilityRecord* record = find(registry, id);
    if (record == nullptr || record->instance != instance)
    {
        return;
    }

    const uint32_t index = static_cast<uint32_t>(record - registry->records);
    for (uint32_t move = index; move + 1U < registry->count; ++move)
    {
        registry->records[move] = registry->records[move + 1U];
    }
    --registry->count;
}

void* resonate_capability_find(ResonateCapabilityRegistry* registry, ResonateId id,
                               uint32_t min_version, uint32_t* out_version)
{
    if (registry == nullptr)
    {
        return nullptr;
    }

    const ResonateCapabilityRecord* record = find(registry, id);
    if (record == nullptr || record->version < min_version)
    {
        return nullptr;
    }

    if (out_version != nullptr)
    {
        *out_version = record->version;
    }
    return record->instance;
}

void* resonate_capability_report_missing(ResonateCapabilityRegistry* registry, ResonateId id,
                                         const char* consumer)
{
    if (registry == nullptr)
    {
        return nullptr;
    }

    char id_text[40] = {};
    formatId(id, id_text, sizeof(id_text));

    char message[192] = {};
    std::snprintf(message, sizeof(message), "capability %s required by %s is not provided", id_text,
                  consumer != nullptr ? consumer : "(unnamed)");
    resonate::detail::hostLog(registry->host, RESONATE_LOG_ERROR, message);
    return nullptr;
}

} // extern "C"
