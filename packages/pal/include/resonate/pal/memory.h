#ifndef RESONATE_PAL_MEMORY_H
#define RESONATE_PAL_MEMORY_H

/*
 * Raw allocation and process memory. The bottom of the engine's memory story;
 * everything above routes through the host allocator injected by the module ABI.
 */

#include <stddef.h>
#include <stdint.h>

#include "resonate/pal/status.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateMemoryStats
{
    uint64_t allocated_bytes;
    uint64_t peak_bytes;
    uint64_t live_blocks;
} ResonateMemoryStats;

/* deallocate takes the size the block was allocated with. */
ResonatePalStatus resonate_pal_memory_allocate(void** out_memory, size_t size, size_t alignment);
void resonate_pal_memory_deallocate(void* memory, size_t size);
ResonatePalStatus resonate_pal_memory_reallocate(void** out_memory, void* memory, size_t old_size,
                                                 size_t new_size, size_t alignment);

/* Emits a report of every live allocation. Debug builds only. */
void resonate_pal_memory_dump_live_allocations(void);
void resonate_pal_memory_get_stats(ResonateMemoryStats* out_stats);

/* Returns RESONATE_PAL_UNSUPPORTED where the platform cannot provide large
   pages, rather than handing back ordinary ones. */
ResonatePalStatus resonate_pal_memory_allocate_large(void** out_memory, size_t size);
void resonate_pal_memory_free_large(void* memory, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_MEMORY_H */
