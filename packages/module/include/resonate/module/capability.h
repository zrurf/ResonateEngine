#ifndef RESONATE_MODULE_CAPABILITY_H
#define RESONATE_MODULE_CAPABILITY_H

/*
 * Capabilities: request/response access to whatever a module offers.
 *
 * A capability is a C struct of function pointers, published by exactly one
 * module. A consumer resolves it once, in on_attach, and holds the plain pointer
 * until on_detach.
 */

#include "resonate/module/abi.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define RESONATE_THREAD_MAIN (1U << 0)
#define RESONATE_THREAD_WORKER (1U << 1)
#define RESONATE_THREAD_ANY (RESONATE_THREAD_MAIN | RESONATE_THREAD_WORKER)

/* Every capability struct leads with this. thread_mask is part of the
   contract: the host checks it in debug builds and reports a hard error. */
typedef struct ResonateCapabilityHeader
{
    uint32_t struct_size;
    uint32_t thread_mask;
} ResonateCapabilityHeader;

/* struct_size is the size of the whole capability struct, not of this header. A
   reader that sees only the header size treats the rest as unreadable. */
#define RESONATE_CAPABILITY_HEADER_INIT(capability_type, thread_mask)                              \
    {(uint32_t)sizeof(capability_type), (thread_mask)}

typedef struct ResonateCapabilityRegistry ResonateCapabilityRegistry;

/* Allocated from the host's allocator. */
ResonateStatus resonate_capability_registry_create(ResonateCapabilityRegistry** out_registry,
                                                   const ResonateHostApi* host);
void resonate_capability_registry_destroy(ResonateCapabilityRegistry* registry);

/* Fails with RESONATE_E_INVALID if the id is already taken. */
ResonateStatus resonate_capability_register(ResonateCapabilityRegistry* registry,
                                            const ResonateCapabilityRecord* record);

/* Unregisters only if the instance matches. */
void resonate_capability_unregister(ResonateCapabilityRegistry* registry, ResonateId id,
                                    void* instance);

/* Returns NULL if nothing provides the id, or if the provider's version is below min_version. */
void* resonate_capability_find(ResonateCapabilityRegistry* registry, ResonateId id,
                               uint32_t min_version, uint32_t* out_version);

/* Logs the missing id and returns NULL. */
void* resonate_capability_report_missing(ResonateCapabilityRegistry* registry, ResonateId id,
                                         const char* consumer);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_MODULE_CAPABILITY_H */
