#ifndef RESONATE_PAL_CHRONO_H
#define RESONATE_PAL_CHRONO_H

/*
 * Monotonic time. Callers measure intervals in ticks and convert at a boundary;
 * the conversion frequency comes from clock_info rather than being assumed.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateTimestamp
{
    uint64_t nanoseconds;
} ResonateTimestamp;

typedef struct ResonateClockInfo
{
    uint64_t frequency;     /* ticks per second */
    uint64_t resolution_ns; /* smallest distinguishable interval */
} ResonateClockInfo;

/* Raw counter. Only differences are meaningful. */
uint64_t resonate_pal_chrono_ticks(void);

ResonateClockInfo resonate_pal_chrono_clock_info(void);

uint64_t resonate_pal_chrono_ticks_to_nanoseconds(uint64_t ticks);
uint64_t resonate_pal_chrono_nanoseconds_to_ticks(uint64_t nanoseconds);

/* Monotonic since an unspecified epoch, not wall-clock time. */
ResonateTimestamp resonate_pal_chrono_now(void);

/* Yields the thread for at least the requested duration. */
void resonate_pal_chrono_sleep(uint64_t nanoseconds);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_CHRONO_H */
