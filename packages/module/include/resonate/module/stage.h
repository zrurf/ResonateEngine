#ifndef RESONATE_MODULE_STAGE_H
#define RESONATE_MODULE_STAGE_H

/*
 * Frame stages and scheduling.
 *
 * A system registers work against a stage and declares the resource groups it
 * reads and writes. Systems run in registration order, and conflicting
 * registrations are reported when they are added.
 */

#include "resonate/module/abi.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum ResonateStage : uint8_t
{
    /* One-shot, before the frame loop starts. */
    RESONATE_STAGE_LOADING = 0,

    RESONATE_STAGE_EARLY_UPDATE = 1, /* input, simulation intent, pending messages */
    RESONATE_STAGE_UPDATE = 2,       /* gameplay logic, animation sampling */
    RESONATE_STAGE_PHYSICS = 3,      /* physics integration */
    RESONATE_STAGE_LATE_UPDATE = 4,  /* camera follow, final transform resolution */
    RESONATE_STAGE_RENDER = 5,       /* draw submission */
    RESONATE_STAGE_PRESENT = 6,      /* present, end of frame */

    RESONATE_STAGE_COUNT = 7
} ResonateStage;

/* Resource groups. Distinct groups are independent. */
typedef uint32_t ResonateResourceGroup;

#define RESONATE_GROUP_DECLARE(name) (1U << (name))

typedef void (*ResonateSystemFn)(void* context, float delta_seconds);

typedef struct ResonateSystemDesc
{
    uint32_t struct_size;
    ResonateStage stage;
    ResonateSystemFn run;

    /* Bitmask of read and write groups. Two systems in the same stage that share
       a group, with at least one of them writing, are reported as conflicting
       when the second is added. Systems in different stages are ordered by the
       schedule, so they are not compared. */
    ResonateResourceGroup reads;
    ResonateResourceGroup writes;

    /* Set when a system must observe the frame's effects immediately, e.g.
       physics. Recorded, but the scheduler runs one stage at a time and does not
       yet act on it. */
    int exclusive_stage;

    void* context;

    /* Names the system in conflict reports and for removal. */
    const char* name;
} ResonateSystemDesc;

typedef struct ResonateScheduler ResonateScheduler;

ResonateStatus resonate_scheduler_create(ResonateScheduler** out_scheduler,
                                         const ResonateHostApi* host);
void resonate_scheduler_destroy(ResonateScheduler* scheduler);

ResonateStatus resonate_scheduler_add_system(ResonateScheduler* scheduler,
                                             const ResonateSystemDesc* desc);

void resonate_scheduler_remove_system(ResonateScheduler* scheduler, const char* name);

/* Runs the systems of one stage, in registration order. */
void resonate_scheduler_run_stage(ResonateScheduler* scheduler, ResonateStage stage,
                                  float delta_seconds);

/* Runs every stage from EARLY_UPDATE through PRESENT; RESONATE_STAGE_LOADING is
   one-shot and not part of the frame. */
void resonate_scheduler_run_frame(ResonateScheduler* scheduler, float delta_seconds);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_MODULE_STAGE_H */
