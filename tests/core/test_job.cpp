#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <resonate/core/job.h>
#include <resonate/pal/sync.h>
#include <resonate/pal/thread.h>

using resonate::JobGroup;
using resonate::JobIndex;
using resonate::JobPriorityHigh;
using resonate::JobPriorityLow;
using resonate::JobPriorityNormal;
using resonate::JobSystem;

namespace
{

constexpr JobGroup GROUP_A = 1U << 0;
constexpr JobGroup GROUP_B = 1U << 1;
constexpr JobGroup GROUP_C = 1U << 2;

/* Records the job's completion rank; each job writes a distinct atomic, so the
   ranks order the completions without racing. */
struct Stamp
{
    std::atomic<std::uint32_t>* rank;
    std::atomic<std::uint32_t>* next;
};

void stampTask(void* context)
{
    auto* stamp = static_cast<Stamp*>(context);
    stamp->rank->store(stamp->next->fetch_add(1, std::memory_order_relaxed),
                       std::memory_order_relaxed);
}

} // namespace

TEST_CASE("a parallel for covers every index exactly once", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();
    REQUIRE(jobs != nullptr);

    std::vector<std::uint32_t> counts(10000, 0);
    const JobIndex index = jobs->submitParallel(
        [](void* context, std::uint32_t begin, std::uint32_t end)
        {
            auto* counts = static_cast<std::vector<std::uint32_t>*>(context);
            for (std::uint32_t i = begin; i < end; ++i)
            {
                (*counts)[i] += 1;
            }
        },
        &counts, static_cast<std::uint32_t>(counts.size()), 0, 0, JobPriorityNormal);
    REQUIRE(index != 0);

    jobs->wait(index);
    REQUIRE(std::all_of(counts.begin(), counts.end(),
                        [](std::uint32_t count) { return count == 1; }));

    delete jobs;
}

TEST_CASE("a single task runs once and an empty parallel for runs nothing", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    std::atomic<std::uint32_t> ran{0};
    const JobIndex single = jobs->submitSingle(
        [](void* context) { static_cast<std::atomic<std::uint32_t>*>(context)->fetch_add(1); },
        &ran, 0, 0, JobPriorityNormal, resonate::JobAffinityThroughput);
    REQUIRE(single != 0);
    jobs->wait(single);
    REQUIRE(ran.load() == 1);

    /* A completed handle, an unknown one and the null handle all return rather
       than block. */
    jobs->wait(single);
    jobs->wait(0);
    jobs->wait((999U << 12) | 3U);

    std::atomic<std::uint32_t> empty{0};
    const JobIndex nothing = jobs->submitParallel(
        [](void* context, std::uint32_t, std::uint32_t)
        { static_cast<std::atomic<std::uint32_t>*>(context)->fetch_add(1); },
        &empty, 0, 0, 0, JobPriorityNormal);
    REQUIRE(nothing != 0);
    jobs->wait(nothing);
    REQUIRE(empty.load() == 0);

    delete jobs;
}

TEST_CASE("jobs writing one group never overlap", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    /* Each writer holds the group and looks for company before leaving, so a
       missing serialisation shows up as two writers inside at once. Under the
       rule the wait is always the full bound: no partner is ever legal. */
    struct Exclusive
    {
        std::atomic<std::uint32_t> inside{0};
        std::atomic<std::uint32_t> worst{0};
    } state;

    for (int i = 0; i < 8; ++i)
    {
        REQUIRE(jobs->submitSingle(
                    [](void* context)
                    {
                        auto* self = static_cast<Exclusive*>(context);
                        const std::uint32_t now = self->inside.fetch_add(1) + 1;
                        std::uint32_t worst = self->worst.load();
                        while (worst < now && !self->worst.compare_exchange_weak(worst, now))
                        {
                        }
                        for (int spin = 0; spin < 200000; ++spin)
                        {
                            resonate_pal_thread_yield();
                        }
                        self->inside.fetch_sub(1);
                    },
                    &state, 0, GROUP_A, JobPriorityNormal,
                    resonate::JobAffinityThroughput) != 0);
    }
    /* The submitting thread stays out of the way for a while, so the workers —
       not this thread's wait — are what runs the writers. */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    jobs->waitAll();
    REQUIRE(state.worst.load() == 1);

    delete jobs;
}

TEST_CASE("readers of one group run concurrently", "[core][job]")
{
    if (resonate_pal_sync_hardware_concurrency() < 2)
    {
        SKIP("needs a second core");
    }

    JobSystem* jobs = resonate::createJobSystem();
    jobs->requestWorkerCount(2);

    /* A reader holds its flag while inside and drops it on the way out, so two
       flags held at once is two threads inside at once. */
    struct Readers
    {
        std::atomic<std::uint32_t> enteredA{0};
        std::atomic<std::uint32_t> enteredB{0};
        std::atomic<bool> saw{false};
    } state;

    const auto readerA = [](void* context)
    {
        auto* self = static_cast<Readers*>(context);
        self->enteredA.fetch_add(1);
        for (int spin = 0; spin < 2000000 && self->enteredB.load() == 0; ++spin)
        {
            resonate_pal_thread_yield();
        }
        if (self->enteredB.load() != 0)
        {
            self->saw.store(true);
        }
        self->enteredA.fetch_sub(1);
    };
    const auto readerB = [](void* context)
    {
        auto* self = static_cast<Readers*>(context);
        self->enteredB.fetch_add(1);
        for (int spin = 0; spin < 2000000 && self->enteredA.load() == 0; ++spin)
        {
            resonate_pal_thread_yield();
        }
        if (self->enteredA.load() != 0)
        {
            self->saw.store(true);
        }
        self->enteredB.fetch_sub(1);
    };

    REQUIRE(jobs->submitSingle(readerA, &state, GROUP_A, 0, JobPriorityNormal,
                               resonate::JobAffinityThroughput) != 0);
    REQUIRE(jobs->submitSingle(readerB, &state, GROUP_A, 0, JobPriorityNormal,
                               resonate::JobAffinityThroughput) != 0);
    /* The second call re-wakes workers that parked again between the two
       submissions, so the readers are not left to one thread's spin. */
    jobs->requestWorkerCount(2);

    jobs->waitAll();
    REQUIRE(state.saw.load());

    delete jobs;
}

TEST_CASE("priorities are layered", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    /* No workers: the waiting thread is the only runner, so the order it drains
       the priority layers in is the order the jobs start in. The count must have
       been observed before submitting — a worker still starting up can otherwise
       take a job on a stale count, and an entry it holds is invisible to every
       other consumer for as long as it keeps it. */
    jobs->requestWorkerCount(0);
    int settled = 0;
    for (int spin = 0; spin < 10000000 && settled < 1000; ++spin)
    {
        settled = jobs->workerCount() == 0 ? settled + 1 : 0;
    }
    REQUIRE(settled == 1000);

    std::vector<char> order(3, '?');
    std::atomic<std::uint32_t> slot{0};
    struct Labeled
    {
        std::vector<char>* order;
        std::atomic<std::uint32_t>* slot;
        char label;
    };
    Labeled labeled[3] = {{&order, &slot, 'L'}, {&order, &slot, 'N'}, {&order, &slot, 'H'}};

    const auto task = [](void* context)
    {
        auto* self = static_cast<const Labeled*>(context);
        const std::uint32_t at = self->slot->fetch_add(1, std::memory_order_relaxed);
        (*self->order)[at] = self->label;
    };

    REQUIRE(jobs->submitSingle(task, &labeled[0], 0, 0, JobPriorityLow,
                               resonate::JobAffinityThroughput) != 0);
    REQUIRE(jobs->submitSingle(task, &labeled[1], 0, 0, JobPriorityNormal,
                               resonate::JobAffinityThroughput) != 0);
    REQUIRE(jobs->submitSingle(task, &labeled[2], 0, 0, JobPriorityHigh,
                               resonate::JobAffinityThroughput) != 0);

    jobs->waitAll();
    REQUIRE(order[0] == 'H');
    REQUIRE(order[1] == 'N');
    REQUIRE(order[2] == 'L');

    delete jobs;
}

TEST_CASE("a slow conflicting job gates a fast one submitted after it", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    std::atomic<std::uint32_t> next{1};
    std::atomic<std::uint32_t> rankA{0};
    std::atomic<std::uint32_t> rankB{0};
    std::atomic<std::uint32_t> rankC{0};
    Stamp stamps[3] = {{&rankA, &next}, {&rankB, &next}, {&rankC, &next}};

    /* A writes the group and stalls before stamping; B writes it too and C
       reads it, both fast. Without the masks B and C would stamp first, which
       is what the ranks must not show. */
    REQUIRE(jobs->submitSingle(
                [](void* context)
                {
                    auto* stamp = static_cast<Stamp*>(context);
                    for (int spin = 0; spin < 200000; ++spin)
                    {
                        resonate_pal_thread_yield();
                    }
                    stamp->rank->store(stamp->next->fetch_add(1, std::memory_order_relaxed),
                                       std::memory_order_relaxed);
                },
                &stamps[0], 0, GROUP_A, JobPriorityNormal,
                resonate::JobAffinityThroughput) != 0);
    REQUIRE(jobs->submitSingle(&stampTask, &stamps[1], 0, GROUP_A, JobPriorityNormal,
                               resonate::JobAffinityThroughput) != 0);
    REQUIRE(jobs->submitSingle(&stampTask, &stamps[2], GROUP_A, 0, JobPriorityNormal,
                               resonate::JobAffinityThroughput) != 0);

    jobs->waitAll();
    REQUIRE(rankA.load() < rankB.load());
    REQUIRE(rankB.load() < rankC.load());

    delete jobs;
}

TEST_CASE("the worker count can be tuned down and up", "[core][job]")
{
    const std::uint32_t hardware = resonate_pal_sync_hardware_concurrency();
    JobSystem* jobs = resonate::createJobSystem();

    struct Registry
    {
        std::mutex mutex;
        std::set<std::uint32_t> workerIds;
    } registry;

    const auto task = [](void* context)
    {
        auto* registry = static_cast<Registry*>(context);
        if (resonate_pal_thread_is_main() == 0U)
        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            registry->workerIds.insert(resonate_pal_thread_id());
        }
    };

    const auto submitBatch = [&](int count)
    {
        for (int i = 0; i < count; ++i)
        {
            REQUIRE(jobs->submitSingle(task, &registry, 0, 0, JobPriorityNormal,
                                       resonate::JobAffinityThroughput) != 0);
        }
        jobs->waitAll();
    };

    /* The count settles as workers reach their park; a worker the request's own
       tokens woke counts awake for a microsecond on its way back in, so the
       check wants the reading stable rather than taken once. */
    const auto settle = [&](std::uint32_t limit)
    {
        int stable = 0;
        for (int spin = 0; spin < 10000000 && stable < 1000; ++spin)
        {
            stable = jobs->workerCount() <= limit ? stable + 1 : 0;
        }
        REQUIRE(stable == 1000);
    };

    jobs->requestWorkerCount(2);
    settle(2);
    submitBatch(64);
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        REQUIRE(registry.workerIds.size() <= 2);
    }

    registry.workerIds.clear();
    const std::uint32_t raised = std::min<std::uint32_t>(hardware, 8);
    jobs->requestWorkerCount(raised);
    settle(raised);
    submitBatch(256);
    {
        /* Only the ceiling is asserted: the waiting thread may legitimately run
           everything itself, so how many workers participated is a service
           level, not an invariant. That the pool runs jobs on more than one
           thread at once is proven where it can be: the readers test. */
        std::lock_guard<std::mutex> lock(registry.mutex);
        REQUIRE(registry.workerIds.size() <= raised);
    }

    delete jobs;
}

TEST_CASE("mixed submissions drain", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    std::atomic<std::uint32_t> singles{0};
    for (int i = 0; i < 200; ++i)
    {
        /* Rotating masks make neighbours conflict without funnelling everything
           through one group. */
        const JobGroup reads = 1U << (i % 3);
        const JobGroup writes = 1U << ((i + 1) % 3);
        REQUIRE(jobs->submitSingle(
                    [](void* context)
                    { static_cast<std::atomic<std::uint32_t>*>(context)->fetch_add(1); },
                    &singles, reads, writes, JobPriorityNormal,
                    resonate::JobAffinityThroughput) != 0);
    }

    /* Twenty parallel fors writing one group: the group serialises the jobs,
       while the slices inside one job stay parallel onto distinct indices. */
    std::vector<std::uint32_t> counts(500, 0);
    for (int i = 0; i < 20; ++i)
    {
        REQUIRE(jobs->submitParallel(
                    [](void* context, std::uint32_t begin, std::uint32_t end)
                    {
                        auto* counts = static_cast<std::vector<std::uint32_t>*>(context);
                        for (std::uint32_t k = begin; k < end; ++k)
                        {
                            (*counts)[k] += 1;
                        }
                    },
                    &counts, static_cast<std::uint32_t>(counts.size()), GROUP_B, GROUP_C,
                    JobPriorityNormal) != 0);
    }

    jobs->waitAll();
    REQUIRE(singles.load() == 200);
    REQUIRE(std::all_of(counts.begin(), counts.end(),
                        [](std::uint32_t count) { return count == 20; }));

    delete jobs;
}

TEST_CASE("destroying with outstanding work drains it", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    std::atomic<std::uint32_t> ran{0};
    for (int i = 0; i < 32; ++i)
    {
        REQUIRE(jobs->submitSingle(
                    [](void* context)
                    { static_cast<std::atomic<std::uint32_t>*>(context)->fetch_add(1); },
                    &ran, 0, 0, JobPriorityNormal, resonate::JobAffinityThroughput) != 0);
    }

    delete jobs;
    REQUIRE(ran.load() == 32);
}
