#ifndef RESONATE_MODULE_STAGE_H
#define RESONATE_MODULE_STAGE_H

/*
 * Frame stages and scheduling.
 *
 * A system registers work against a stage and declares the resource groups it
 * reads and writes. The systems of one stage run as a dependency graph: two
 * systems whose declared groups do not conflict may run at the same time, and
 * ones that conflict — or that declare no group at all — run one after another
 * in registration order. A stage is a barrier: it returns once every one of its
 * systems has completed.
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

/* A system that works on the world: called instead of `run` when set. The world
   and the command buffer are the host's — a system records structural changes
   into `commands` and the frame's sync points play it. */
typedef void (*ResonateSystemWorldFn)(void* context, ResonateWorld* world,
                                      ResonateCommands* commands, float delta_seconds);

typedef struct ResonateSystemDesc
{
    uint32_t struct_size;
    ResonateStage stage;
    ResonateSystemFn run;

    /* Bitmask of read and write groups. Two systems in the same stage that share
       a group, with at least one of them writing, are serialised in registration
       order and reported when the second is added. Systems in different stages
       are separated by the stage barrier. A system that declares no group is
       serialised with every other system of its stage: running in parallel is
       opt-in, so declaring nothing is not a claim of independence. */
    ResonateResourceGroup reads;
    ResonateResourceGroup writes;

    /* Set when a system must not share its stage with anything, e.g. physics:
       it starts only after every system registered before it in the stage has
       completed, and every system after it waits for its completion. Takes
       effect in addition to the resource groups. */
    int exclusive_stage;

    void* context;

    /* Names the system in conflict reports and for removal. */
    const char* name;

    /*
     * The world-aware form, appended after the fields above: a descriptor from
     * a header older than these keeps working, because they are read only when
     * struct_size covers them (a zero size claims nothing past the base).
     *
     * run_world, when set, is called instead of run with the host-filled world
     * handle and this system's own command buffer. Structural changes go into
     * that buffer and play at the frame's sync points. A world-aware system is
     * serialised with every other system of its stage, groups or not: recording
     * shares the world's slot table, which takes one thread.
     */
    ResonateSystemWorldFn run_world;

    /* Filled by the host at registration, never by the caller. */
    ResonateWorld* world;
    ResonateCommands* commands;
} ResonateSystemDesc;

/* How much of a descriptor is always there, whatever the writer's size says. */
#define RESONATE_SYSTEM_DESC_BASE_SIZE ((uint32_t)offsetof(ResonateSystemDesc, run_world))

/* Whether the writer's struct_size covers the world-aware tail. Anything that
   claims less — an older descriptor, a zero — reads as the base alone, so the
   reader never touches a tail the writer did not write. */
static inline int resonate_system_desc_has_world(const ResonateSystemDesc* desc)
{
    return desc->struct_size >= (uint32_t)sizeof(ResonateSystemDesc);
}

typedef struct ResonateScheduler ResonateScheduler;

ResonateStatus resonate_scheduler_create(ResonateScheduler** out_scheduler,
                                         const ResonateHostApi* host);
void resonate_scheduler_destroy(ResonateScheduler* scheduler);

ResonateStatus resonate_scheduler_add_system(ResonateScheduler* scheduler,
                                             const ResonateSystemDesc* desc);

void resonate_scheduler_remove_system(ResonateScheduler* scheduler, const char* name);

/* Removes the first system whose name, run function and context all match. The
   host withdraws one module's registrations through this: a name can be shared,
   the full triple cannot, so the removal cannot hit another module's
   same-named system. */
void resonate_scheduler_remove_system_exact(ResonateScheduler* scheduler, const char* name,
                                            ResonateSystemFn run, void* context);

/* Runs one stage's dependency graph and returns when all of its systems have
   completed. */
void resonate_scheduler_run_stage(ResonateScheduler* scheduler, ResonateStage stage,
                                  float delta_seconds);

/* Runs every stage from EARLY_UPDATE through PRESENT; RESONATE_STAGE_LOADING is
   one-shot and not part of the frame. This is the schedule alone: the command
   buffers a world-aware system records into are played by the host's frame
   (ModuleHost::runFrame), which is what puts the sync points between the
   stages. */
void resonate_scheduler_run_frame(ResonateScheduler* scheduler, float delta_seconds);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_MODULE_STAGE_H */
