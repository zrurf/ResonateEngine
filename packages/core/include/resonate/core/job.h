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
 * waiting main thread from idling while workers are busy. A submission into a
 * full pool participates too: when every slot is held by work that has not
 * retired, the submitter runs queued work until one frees, so a burst of
 * submissions slows down rather than dropping a job. Bodies run on worker
 * threads or on a waiting or submitting thread and must not throw.
 *
 * Nested submission: a job submitted from within a job's body is that task's
 * own work and is published at once, without joining the dependency graph. The
 * task holds the groups it declared until it retires, so everything that
 * conflicts with it is either excluded already or ordered behind it — and a
 * nested job that waited for work ordered behind its own parent would deadlock.
 * Nested bodies run under the parent's exclusivity; the masks passed with one
 * do not order it against anything.
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

/* Scheduling hint for heterogeneous cores. A job's entries are published to
   the workers of its class — Background to the efficiency set, Latency to the
   performance set, Throughput everywhere — and stealing is the overflow: an
   idle worker of the wrong class takes an entry when nothing of its own is
   left. On a homogeneous pool every class publishes everywhere. */
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

    /* Workers the pool was created with: the widest count requestWorkerCount
       can reach. */
    virtual std::uint32_t workerCapacity() const = 0;

    /* Workers keep working; idle ones park until the pool holds `count` active
       workers (clamped to the pool created at startup). */
    virtual void requestWorkerCount(std::uint32_t count) = 0;
};

/* Creates the pool with one worker per logical processor this process may run
   on, or with `worker_limit` workers when that is smaller (zero means no
   limit). The workers are placed on the topology's processors, fastest first,
   and all of them start active. Destroy with delete: the destructor drains
   every submitted job, then wakes and joins the workers. */
JobSystem* createJobSystem(std::uint32_t worker_limit = 0);

} // namespace resonate

#endif /* RESONATE_CORE_JOB_H */
