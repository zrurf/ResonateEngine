#include <resonate/core/job.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <vector>

#include <resonate/pal/sync.h>
#include <resonate/pal/thread.h>

namespace resonate
{
namespace
{

/* Jobs in flight are slots: the low bits of a JobIndex name the slot, the rest
   is a generation that turns a recycled slot into "already completed" for a
   stale handle instead of a wait on somebody else's job. */
constexpr std::uint32_t kSlotBits = 12;
constexpr std::uint32_t kSlotCount = 1U << kSlotBits;
constexpr std::uint32_t kGroupBits = 32;
constexpr std::uint32_t kNoWorker = 0xFFFFFFFFU;
constexpr std::uint32_t kParkSpins = 4;

/* RAII over a PAL mutex. */
class Lock
{
  public:
    explicit Lock(ResonateMutex& mutex) : mutex_(&mutex)
    {
        resonate_pal_sync_lock_mutex(mutex_);
    }
    ~Lock()
    {
        resonate_pal_sync_unlock_mutex(mutex_);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

  private:
    ResonateMutex* mutex_;
};

/* One conflict-and-completion unit. Queued work is an entry naming a job; the
   entry's runner claims chunks of [0, total) until none are left, so a parallel
   for spreads across every worker holding one of its entries while staying a
   single node in the dependency graph.

   A job is reclaimed only when its chunks have run and every entry has been
   consumed, which the single work counter below records, so an entry popped
   later never names a recycled job. */
struct Job
{
    JobFunction slice = nullptr;
    JobTask task = nullptr;
    void* context = nullptr;
    JobGroup reads = 0;
    JobGroup writes = 0;
    JobPriority priority = JobPriorityNormal;
    JobAffinity affinity = JobAffinityThroughput;
    std::uint32_t total = 0;
    std::uint32_t chunk = 1;
    std::uint32_t slot = 0;

    std::atomic<std::uint32_t> next{0}; /* next chunk begin handed out */

    /* Chunks not yet run plus entries not yet consumed. Every participant does
       exactly one decrement per chunk it runs and one for the entry it took,
       and the decrement that reaches zero retires the job — so nobody touches
       the slot after it can have been recycled. */
    std::atomic<std::uint32_t> work{0};

    /* Guarded by the dependency mutex. */
    std::uint32_t depCount = 0;
    std::uint32_t generation = 1;
    bool live = false;
    std::vector<Job*> dependents;
};

bool conflicts(const Job& a, const Job& b)
{
    return ((a.writes & (b.reads | b.writes)) | (b.writes & a.reads)) != 0U;
}

class JobSystemImpl final : public JobSystem
{
  public:
    JobSystemImpl()
    {
        if (resonate_pal_sync_create_semaphore(&semaphore_, 0) != RESONATE_PAL_OK ||
            resonate_pal_sync_create_semaphore(&excessSemaphore_, 0) != RESONATE_PAL_OK ||
            resonate_pal_sync_create_mutex(&dagMutex_, 0) != RESONATE_PAL_OK ||
            resonate_pal_sync_create_mutex(&globalMutex_, 0) != RESONATE_PAL_OK)
        {
            return;
        }
        sync_ = true;

        const std::uint32_t hardware = resonate_pal_sync_hardware_concurrency();
        target_.store(hardware, std::memory_order_relaxed);
        workers_.resize(hardware);
        for (std::uint32_t index = 0; index < hardware; ++index)
        {
            workers_[index].system = this;
            workers_[index].index = index;
            if (resonate_pal_sync_create_mutex(&workers_[index].mutex, 0) != RESONATE_PAL_OK)
            {
                break;
            }
            ResonateThreadHandle thread = {};
            /* Stack size 0 is the platform default; affinity stays with the
               scheduler until topology probing places workers by role. */
            if (resonate_pal_thread_create(&thread, &entry, &workers_[index], 0, -1) !=
                RESONATE_PAL_OK)
            {
                resonate_pal_sync_destroy_mutex(&workers_[index].mutex);
                break;
            }
            workers_[index].thread = thread;
            ++workerCount_;
        }

        /* Threads start running the moment they are created; the pool they land
           in is only fully built here. */
        ready_.store(true, std::memory_order_release);
    }

    ~JobSystemImpl() override
    {
        if (!sync_)
        {
            return;
        }

        waitAll();
        stopping_.store(true, std::memory_order_release);
        resonate_pal_sync_signal_semaphore(&semaphore_, workerCount_);
        resonate_pal_sync_signal_semaphore(&excessSemaphore_, workerCount_);

        /* Every join first: a worker that has not yet observed the stop can still
           be stealing from its neighbours' queues, so no per-worker storage may
           be destroyed while any of them is alive. */
        for (std::uint32_t index = 0; index < workerCount_; ++index)
        {
            resonate_pal_thread_join(&workers_[index].thread);
        }
        for (std::uint32_t index = 0; index < workerCount_; ++index)
        {
            resonate_pal_sync_destroy_mutex(&workers_[index].mutex);
        }
        resonate_pal_sync_destroy_mutex(&globalMutex_);
        resonate_pal_sync_destroy_mutex(&dagMutex_);
        resonate_pal_sync_destroy_semaphore(&semaphore_);
        resonate_pal_sync_destroy_semaphore(&excessSemaphore_);
    }

    JobSystemImpl(const JobSystemImpl&) = delete;
    JobSystemImpl& operator=(const JobSystemImpl&) = delete;

    JobIndex submitParallel(JobFunction body, void* context, std::uint32_t count, JobGroup reads,
                            JobGroup writes, JobPriority priority) override
    {
        return submit(body, nullptr, context, count, reads, writes, priority,
                      JobAffinityThroughput);
    }

    JobIndex submitSingle(JobTask task, void* context, JobGroup reads, JobGroup writes,
                          JobPriority priority, JobAffinity affinity) override
    {
        return submit(nullptr, task, context, 1, reads, writes, priority, affinity);
    }

    void wait(JobIndex job) override
    {
        for (;;)
        {
            if (completed(job))
            {
                return;
            }
            if (Job* next = findJob(kNoWorker))
            {
                run(next);
                continue;
            }
            resonate_pal_thread_yield();
        }
    }

    void waitAll() override
    {
        for (;;)
        {
            if (Job* next = findJob(kNoWorker))
            {
                run(next);
                continue;
            }
            if (incomplete_.load(std::memory_order_acquire) == 0)
            {
                return;
            }
            resonate_pal_thread_yield();
        }
    }

    std::uint32_t workerCount() const override
    {
        return awake_.load(std::memory_order_relaxed);
    }

    void requestWorkerCount(std::uint32_t count) override
    {
        count = std::min(count, workerCount_);
        target_.store(count, std::memory_order_relaxed);

        /* Raising wakes excess workers; the ones now inside the requested set
           find the gate open, the rest come straight back here. */
        const std::uint32_t awake = awake_.load(std::memory_order_seq_cst);
        if (count > awake)
        {
            resonate_pal_sync_signal_semaphore(&excessSemaphore_, count - awake);
        }
    }

  private:
    struct Worker
    {
        JobSystemImpl* system = nullptr;
        std::uint32_t index = 0;
        ResonateThreadHandle thread = {};
        ResonateMutex mutex = {};
        std::deque<Job*> queues[3]; /* indexed by JobPriority */
    };

    static void entry(void* argument)
    {
        auto* worker = static_cast<Worker*>(argument);
        worker->system->workerLoop(worker->index);
    }

    void workerLoop(std::uint32_t index)
    {
        while (!ready_.load(std::memory_order_acquire))
        {
            resonate_pal_thread_yield();
        }
        awake_.fetch_add(1, std::memory_order_relaxed);
        for (;;)
        {
            if (stopping_.load(std::memory_order_acquire))
            {
                break;
            }
            /* Shrinking parks the workers beyond the requested count, the first
               `target` by index keeping their queues. */
            if (index >= target_.load(std::memory_order_acquire))
            {
                excessPark(index);
                continue;
            }
            if (tryRun(index))
            {
                continue;
            }
            park(index);
        }
        awake_.fetch_sub(1, std::memory_order_relaxed);
    }

    /* Takes one job and runs it, if the count allows this worker to work at
       all. The gate is read before the job is taken, so a worker the count has
       already excluded never holds an entry that a legal consumer could have
       run — nothing is taken and put back. A worker that read the count before
       a shrink may still run the job it is holding: the adjustment takes effect
       at the next scheduling decision, as the thread model promises. */
    bool tryRun(std::uint32_t index)
    {
        if (index >= target_.load(std::memory_order_acquire))
        {
            return false;
        }

        Job* job = findJob(index);
        if (job == nullptr)
        {
            return false;
        }
        run(job);
        return true;
    }

    /* The active set parks on the pool semaphore, the excess on its own, so a
       wake meant for the active set is never consumed by a worker whose index
       puts it beyond the requested count. */
    void excessPark(std::uint32_t index)
    {
        awake_.fetch_sub(1, std::memory_order_seq_cst);
        if (index < target_.load(std::memory_order_relaxed))
        {
            /* The count rose while this worker was on its way in. */
            awake_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        resonate_pal_sync_wait_semaphore(&excessSemaphore_);
        awake_.fetch_add(1, std::memory_order_relaxed);
    }

    /* Idle workers spin briefly, then park on the semaphore. The awake count
       drops (seq_cst) before the final scan, so a submission that reads the
       worker as parked has already published its entry into a queue that scan
       visits, and one that reads it as awake signals the semaphore it is about
       to block on. A worker beyond the target never reaches the scan: it parks
       on the excess semaphore, which only a raise signals. */
    void park(std::uint32_t index)
    {
        if (index >= target_.load(std::memory_order_relaxed))
        {
            excessPark(index);
            return;
        }

        for (std::uint32_t spin = 0; spin < kParkSpins; ++spin)
        {
            resonate_pal_thread_yield();
            if (tryRun(index))
            {
                return;
            }
        }

        awake_.fetch_sub(1, std::memory_order_seq_cst);
        if (tryRun(index))
        {
            awake_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        resonate_pal_sync_wait_semaphore(&semaphore_);
        awake_.fetch_add(1, std::memory_order_relaxed);
    }

    /* Priority-layered: every High source before any Normal one, and so on. Own
       queue first for locality, then a round-robin steal, then the fallback
       queue that exists for a pool whose threads could not be created. */
    Job* findJob(std::uint32_t self)
    {
        for (int priority = JobPriorityHigh; priority >= JobPriorityLow; --priority)
        {
            if (self != kNoWorker)
            {
                Worker& worker = workers_[self];
                Lock lock(worker.mutex);
                if (!worker.queues[priority].empty())
                {
                    Job* job = worker.queues[priority].back();
                    worker.queues[priority].pop_back();
                    queued_.fetch_sub(1, std::memory_order_release);
                    return job;
                }
            }

            const std::uint32_t count = workerCount_;
            const std::uint32_t start = stealCursor_.fetch_add(1, std::memory_order_relaxed);
            for (std::uint32_t step = 0; step < count; ++step)
            {
                const std::uint32_t victim = (start + step) % count;
                if (victim == self)
                {
                    continue;
                }
                Worker& worker = workers_[victim];
                Lock lock(worker.mutex);
                if (!worker.queues[priority].empty())
                {
                    Job* job = worker.queues[priority].front();
                    worker.queues[priority].pop_front();
                    queued_.fetch_sub(1, std::memory_order_release);
                    return job;
                }
            }

            if (workerCount_ == 0)
            {
                Lock lock(globalMutex_);
                if (!global_[priority].empty())
                {
                    Job* job = global_[priority].front();
                    global_[priority].pop_front();
                    queued_.fetch_sub(1, std::memory_order_release);
                    return job;
                }
            }
        }
        return nullptr;
    }

    void run(Job* job)
    {
        if (job->task != nullptr)
        {
            job->task(job->context);
            job->work.fetch_sub(1, std::memory_order_acq_rel);
        }
        else
        {
            for (;;)
            {
                const std::uint32_t begin =
                    job->next.fetch_add(job->chunk, std::memory_order_acq_rel);
                if (begin >= job->total)
                {
                    break;
                }
                job->slice(job->context, begin, std::min(begin + job->chunk, job->total));
                job->work.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        /* The entry is consumed whether or not its runner claimed a chunk, and
           the decrement that reaches zero is this job's only closer: comparing
           the value the decrement returned touches no memory of a slot that a
           concurrent runner has just retired. */
        if (job->work.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            finish(job);
        }
    }

    JobIndex submit(JobFunction slice, JobTask task, void* context, std::uint32_t count,
                    JobGroup reads, JobGroup writes, JobPriority priority, JobAffinity affinity)
    {
        Lock lock(dagMutex_);
        Job* job = takeSlot();
        if (job == nullptr)
        {
            return 0;
        }

        job->slice = slice;
        job->task = task;
        job->context = context;
        job->reads = reads;
        job->writes = writes;
        job->priority = priority;
        job->affinity = affinity;
        job->total = count;
        job->chunk = chunkFor(count);

        /* A zero-width parallel for runs nothing, so it conflicts with nothing
           and completes as it stands. */
        if (count == 0)
        {
            const JobIndex index = indexOf(*job);
            releaseSlot(job);
            return index;
        }

        job->next.store(0, std::memory_order_relaxed);
        job->work.store((count + job->chunk - 1) / job->chunk, std::memory_order_relaxed);
        job->dependents.clear();

        /* Dependencies come from incomplete jobs sharing a conflicting group.
           The scan, the dependency lists and the publish below share one mutex,
           so a job counted as a dependency cannot complete before this
           submission has registered against it, and a job left ready here
           cannot be turned into somebody's dependency before its entry is in a
           queue. */
        std::vector<Job*> deps;
        const JobGroup touched = reads | writes;
        for (std::uint32_t bit = 0; bit < kGroupBits; ++bit)
        {
            if ((touched & (1U << bit)) == 0U)
            {
                continue;
            }
            for (Job* pending : groups_[bit])
            {
                if (conflicts(*pending, *job) &&
                    std::find(deps.begin(), deps.end(), pending) == deps.end())
                {
                    deps.push_back(pending);
                }
            }
        }
        job->depCount = static_cast<std::uint32_t>(deps.size());
        for (Job* pending : deps)
        {
            pending->dependents.push_back(job);
        }
        for (std::uint32_t bit = 0; bit < kGroupBits; ++bit)
        {
            if ((touched & (1U << bit)) != 0U)
            {
                groups_[bit].push_back(job);
            }
        }
        incomplete_.fetch_add(1, std::memory_order_release);

        const JobIndex index = indexOf(*job);
        if (deps.empty())
        {
            publish(job);
        }
        return index;
    }

    /* Queues one entry per worker, round-robin from a shared cursor, so a
       parallel for reaches as many workers as it has chunks; a pool without
       threads falls back to its global queue, which only a waiting thread
       drains. Called under the dependency mutex, which is what keeps a ready
       job from gaining a dependency before its entry is reachable. */
    void publish(Job* job)
    {
        const std::uint32_t queue = queueOf(job->priority);
        const std::uint32_t entries =
            std::min((job->total + job->chunk - 1) / job->chunk, workerCount_);
        job->work.fetch_add(entries, std::memory_order_relaxed);
        for (std::uint32_t entry = 0; entry < entries; ++entry)
        {
            Worker& worker =
                workers_[handout_.fetch_add(1, std::memory_order_relaxed) % workerCount_];
            Lock lock(worker.mutex);
            worker.queues[queue].push_back(job);
            queued_.fetch_add(1, std::memory_order_release);
        }
        if (entries == 0)
        {
            job->work.fetch_add(1, std::memory_order_relaxed);
            Lock lock(globalMutex_);
            global_[queue].push_back(job);
            queued_.fetch_add(1, std::memory_order_release);
        }

        /* Wake against the backlog, not this publish: a burst of submissions
           while the workers are still spinning must wake them once work is
           visible, and a parked worker must not sleep behind queued entries. */
        const std::uint32_t want =
            std::min(queued_.load(std::memory_order_acquire),
                     target_.load(std::memory_order_relaxed));

        /* The seq_cst load pairs with the seq_cst decrement in park(): a worker
           this load reads as awake is past its decrement, so its final scan
           reaches everything pushed above. */
        const std::uint32_t awake = awake_.load(std::memory_order_seq_cst);
        if (want > awake)
        {
            resonate_pal_sync_signal_semaphore(&semaphore_, want - awake);
        }
    }

    void finish(Job* job)
    {
        Lock lock(dagMutex_);
        const JobGroup touched = job->reads | job->writes;
        for (std::uint32_t bit = 0; bit < kGroupBits; ++bit)
        {
            if ((touched & (1U << bit)) == 0U)
            {
                continue;
            }
            std::vector<Job*>& pending = groups_[bit];
            pending.erase(std::find(pending.begin(), pending.end(), job));
        }
        for (Job* dependent : job->dependents)
        {
            if (--dependent->depCount == 0)
            {
                publish(dependent);
            }
        }
        job->dependents.clear();
        releaseSlot(job);
        incomplete_.fetch_sub(1, std::memory_order_release);
    }

    std::uint32_t chunkFor(std::uint32_t count) const
    {
        if (count <= 1)
        {
            return 1;
        }
        const std::uint32_t workers = std::max(target_.load(std::memory_order_relaxed), 1U);
        const std::uint32_t pieces = workers * 4U;
        return (count + pieces - 1) / pieces;
    }

    /* Both under the dependency mutex. */
    Job* takeSlot()
    {
        std::uint32_t slot = 0;
        if (!freeSlots_.empty())
        {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
        }
        else if (slotsTaken_ < kSlotCount)
        {
            slot = slotsTaken_++;
        }
        else
        {
            return nullptr;
        }

        Job* job = &nodes_[slot];
        job->slot = slot;
        job->live = true;
        return job;
    }

    void releaseSlot(Job* job)
    {
        job->live = false;
        job->generation += 1;
        freeSlots_.push_back(job->slot);
    }

    std::uint32_t indexOf(const Job& job) const
    {
        return job.slot | (job.generation << kSlotBits);
    }

    bool completed(JobIndex index)
    {
        const std::uint32_t slot = index & (kSlotCount - 1);
        Lock lock(dagMutex_);
        const Job& job = nodes_[slot];
        return !job.live || job.generation != (index >> kSlotBits);
    }

    static std::uint32_t queueOf(JobPriority priority)
    {
        return priority <= JobPriorityHigh ? static_cast<std::uint32_t>(priority)
                                           : static_cast<std::uint32_t>(JobPriorityNormal);
    }

    /* One allocation, constructed in place: Job carries atomics and cannot be
       moved, so the pool never grows. */
    std::vector<Job> nodes_ = std::vector<Job>(kSlotCount);
    std::vector<std::uint32_t> freeSlots_;
    std::uint32_t slotsTaken_ = 0;
    std::vector<Job*> groups_[kGroupBits]; /* incomplete jobs per resource group */

    std::vector<Worker> workers_;
    std::uint32_t workerCount_ = 0; /* threads actually created */
    std::deque<Job*> global_[3];

    ResonateMutex dagMutex_ = {};
    ResonateMutex globalMutex_ = {};
    ResonateSemaphore semaphore_ = {};       /* the active set parks here */
    ResonateSemaphore excessSemaphore_ = {}; /* workers beyond the target */
    bool sync_ = false;

    std::atomic<std::uint32_t> awake_{0};
    std::atomic<std::uint32_t> queued_{0}; /* entries sitting in any queue */
    std::atomic<std::uint32_t> target_{1};
    std::atomic<std::uint32_t> incomplete_{0};
    std::atomic<std::uint32_t> stealCursor_{0};
    std::atomic<std::uint32_t> handout_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> ready_{false};
};

} // namespace

JobSystem* createJobSystem()
{
    return new JobSystemImpl();
}

} // namespace resonate
