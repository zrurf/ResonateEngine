#ifndef RESONATE_PAL_SYNC_H
#define RESONATE_PAL_SYNC_H

/*
 * Synchronisation primitives. Handles are opaque and must be created before use;
 * the job system and the module host are the intended callers.
 */

#include <stdint.h>

#include "resonate/pal/status.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateMutex
{
    void* handle;
} ResonateMutex;

typedef struct ResonateConditionVariable
{
    void* handle;
} ResonateConditionVariable;

typedef struct ResonateReadWriteLock
{
    void* handle;
} ResonateReadWriteLock;

typedef struct ResonateSemaphore
{
    void* handle;
} ResonateSemaphore;

ResonatePalStatus resonate_pal_sync_create_mutex(ResonateMutex* out_mutex, int recursive);
void resonate_pal_sync_destroy_mutex(ResonateMutex* mutex);
void resonate_pal_sync_lock_mutex(ResonateMutex* mutex);
void resonate_pal_sync_unlock_mutex(ResonateMutex* mutex);

/* The caller holds mutex; wait releases it while blocked and reacquires it before
   returning. */
ResonatePalStatus resonate_pal_sync_create_condition(ResonateConditionVariable* out_condition);
void resonate_pal_sync_destroy_condition(ResonateConditionVariable* condition);
void resonate_pal_sync_wait(ResonateConditionVariable* condition, ResonateMutex* mutex);
void resonate_pal_sync_signal(ResonateConditionVariable* condition);
void resonate_pal_sync_broadcast(ResonateConditionVariable* condition);

ResonatePalStatus resonate_pal_sync_create_read_write_lock(ResonateReadWriteLock* out_lock);
void resonate_pal_sync_destroy_read_write_lock(ResonateReadWriteLock* lock);
void resonate_pal_sync_lock_shared(ResonateReadWriteLock* lock);
void resonate_pal_sync_lock_exclusive(ResonateReadWriteLock* lock);
void resonate_pal_sync_unlock_read_write_lock(ResonateReadWriteLock* lock);

ResonatePalStatus resonate_pal_sync_create_semaphore(ResonateSemaphore* out_semaphore,
                                                     uint32_t initial_count);
void resonate_pal_sync_destroy_semaphore(ResonateSemaphore* semaphore);
void resonate_pal_sync_wait_semaphore(ResonateSemaphore* semaphore);
void resonate_pal_sync_signal_semaphore(ResonateSemaphore* semaphore, uint32_t count);

/* Number of usable hardware threads, clamped to at least one. */
uint32_t resonate_pal_sync_hardware_concurrency(void);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_SYNC_H */
