#ifndef RESONATE_CORE_JOB_H
#define RESONATE_CORE_JOB_H

#include <cstdint>

namespace resonate
{

/*
 * The execution substrate under the frame schedule: a parked worker pool with a
 * work-stealing queue per worker, layered by priority, plus the resource-group
 * rule ResonateSystemDesc declares in resonate/module/stage.h.
 *
 * Jobs carry (reads, writes) group masks. Two jobs sharing a group with at
 * least one of them writing are serialised — the later submission cannot start
 * before the earlier one completes — so a caller submits freely and the masks
 * decide what runs concurrently. Readers of one group run in parallel.
 *
 * Submission is fire-and-forget; the returned index is a handle for waiting,
 * where a stale or unknown index reads as already completed. Waiting
 * participates: the calling thread runs queued work, which is what keeps a
 * waiting main thread from idling while workers are busy. Bodies run on worker
 * threads or on a waiting thread and must not throw.
 */

using JobIndex = std::uint32_t;

/* Called once per slice of [begin, end). */
using JobFunction = void (*)(void* context, std::uint32_t begin, std::uint32_t end);

/* Called once. */
using JobTask = void (*)(void* context);

/* Resource groups, the convention ResonateSystemDesc declares in
   resonate/module/stage.h: distinct bits are independent, and two jobs sharing a
   bit with at least one of them writing are serialised. */
using JobGroup = std::uint32_t;

enum JobPriority : std::uint8_t
{
    JobPriorityLow = 0,
    JobPriorityNormal = 1,
    JobPriorityHigh = 2,
};

/* Scheduling hint for heterogeneous cores, recorded per job. Consumed once
   topology probing lands (02-pal-platform-config.md §3.1); until then every
   worker is interchangeable. */
enum JobAffinity : std::uint8_t
{
    JobAffinityLatency = 0,    /* frame-critical path */
    JobAffinityThroughput = 1, /* default */
    JobAffinityBackground = 2, /* io, decompression, compilation */
};

class JobSystem
{
  public:
    virtual ~JobSystem() = default;

    /* Runs body over [0, count) in chunks the pool sizes from the worker target.
       Slices may run concurrently and in any order; count of zero is legal and
       runs nothing. */
    virtual JobIndex submitParallel(JobFunction body, void* context, std::uint32_t count,
                                    JobGroup reads, JobGroup writes, JobPriority priority) = 0;

    /* Runs task once. */
    virtual JobIndex submitSingle(JobTask task, void* context, JobGroup reads, JobGroup writes,
                                  JobPriority priority, JobAffinity affinity) = 0;

    /* Runs jobs until `job` has completed. */
    virtual void wait(JobIndex job) = 0;

    /* Runs jobs until every submitted job has completed. */
    virtual void waitAll() = 0;

    /* Workers currently out of their park; settles toward the requested count as
       workers run out of work. */
    virtual std::uint32_t workerCount() const = 0;

    /* Workers keep working; idle ones park until the pool holds `count` active
       workers (clamped to the pool created at startup). */
    virtual void requestWorkerCount(std::uint32_t count) = 0;
};

/* Creates the pool with one worker per hardware thread. Destroy with delete:
   the destructor drains every submitted job, then wakes and joins the workers. */
JobSystem* createJobSystem();

} // namespace resonate

#endif /* RESONATE_CORE_JOB_H */
