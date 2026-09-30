#ifndef RESONATE_CORE_JOB_H
#define RESONATE_CORE_JOB_H

#include <cstdint>

namespace resonate
{

/*
 * Jobs are fire-and-forget: a caller never blocks on a handle, and the access
 * masks declared at submission are what the scheduler orders by.
 *
 * The interface is the seam; there is no pool behind it yet, which is why
 * nothing here hands out an instance. Adding one is a pool plus the conflict
 * rule below, not a change to a caller.
 */

using JobIndex = std::uint32_t;

/* Called once per slice of [begin, end). */
using JobFunction = void (*)(void* context, std::uint32_t begin, std::uint32_t end);

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

class JobSystem
{
  public:
    virtual ~JobSystem() = default;

    /* Splits [0, count) across the pool and runs body on each slice; the return
       value is a handle for tracing. */
    virtual JobIndex submitParallel(JobFunction body, void* context, std::uint32_t count,
                                    JobGroup reads, JobGroup writes, JobPriority priority) = 0;

    /* Blocks until every job submitted so far has completed. */
    virtual void wait() = 0;

    virtual std::uint32_t workerCount() const = 0;
};

} // namespace resonate

#endif /* RESONATE_CORE_JOB_H */
