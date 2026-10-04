#ifndef RESONATE_PAL_THREAD_H
#define RESONATE_PAL_THREAD_H

/*
 * Thread creation and affinity. The job system is the only component that should
 * call these directly.
 */

#include <stddef.h>
#include <stdint.h>

#include "resonate/pal/status.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateThreadHandle
{
    void* handle;
} ResonateThreadHandle;

/* Runs on a new thread with the argument passed through untouched. A stack_size
   of 0 selects the platform default; an affinity of -1 leaves placement to the
   scheduler. Any other affinity is a request the new thread makes for itself,
   since placement can only be applied to the calling thread: one the platform
   declines leaves the thread to the scheduler rather than failing the call. An
   index outside the platform's core mask is refused before a thread exists. */
typedef void (*ResonateThreadEntry)(void* argument);

ResonatePalStatus resonate_pal_thread_create(ResonateThreadHandle* out_thread,
                                             ResonateThreadEntry entry, void* argument,
                                             uint32_t stack_size, int32_t affinity);

/* Releases the handle, detaching a thread that was never joined. May be called
   before or after join; after, there is nothing left to release. */
void resonate_pal_thread_destroy(ResonateThreadHandle* thread);

/* Blocks until the thread returns, then releases the handle. */
ResonatePalStatus resonate_pal_thread_join(ResonateThreadHandle* thread);
void resonate_pal_thread_yield(void);

/* A scheduling hint for the calling thread, applied the same way placement is:
   by the thread itself. A level the platform declines leaves the thread as it
   was — a hint is not a contract and has nothing to report but the refusal. */
typedef enum ResonateThreadPriority
{
    RESONATE_PAL_THREAD_PRIORITY_LOW = 0, /* below normal: background work */
    RESONATE_PAL_THREAD_PRIORITY_NORMAL = 1,
    RESONATE_PAL_THREAD_PRIORITY_HIGH = 2 /* above normal: frame-critical work */
} ResonateThreadPriority;

void resonate_pal_thread_set_priority(ResonateThreadPriority priority);

/* The logical processor the calling thread is on right now, or
   RESONATE_PAL_THREAD_CPU_UNKNOWN. The value is one of the topology's core
   ids; a thread the platform did not place may move between calls. */
#define RESONATE_PAL_THREAD_CPU_UNKNOWN 0xFFFFFFFFu
uint32_t resonate_pal_thread_current_cpu(void);

/* Unique among live threads; the value may be reused once a thread ends.
   is_main returns 1 or 0. */
uint32_t resonate_pal_thread_id(void);
uint32_t resonate_pal_thread_is_main(void);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_THREAD_H */
