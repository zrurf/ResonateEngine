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

    /* One batch warms the pool before the measurement. A worker that read the
       target before the shrink is allowed to run the one job its next decision
       picks up — the adjustment takes effect at the next decision, which the
       thread model states — so a straggler can legitimately appear once; the
       warm-up lets it do that and park before the run below is counted. */
    submitBatch(64);
    registry.workerIds.clear();

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

TEST_CASE("a full slot pool slows submission instead of dropping the job", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    /* No workers: this thread is the only runner, so nothing retires until the
       submissions themselves or the drain reaches it, and the number of
       outstanding jobs reaches the pool's own slot count. */
    jobs->requestWorkerCount(0);
    int settled = 0;
    for (int spin = 0; spin < 10000000 && settled < 1000; ++spin)
    {
        settled = jobs->workerCount() == 0 ? settled + 1 : 0;
    }
    REQUIRE(settled == 1000);

    /* Past the slot count a submission has no slot left. It must run queued work
       until one frees, not return a handle that reads as already completed with
       the job silently gone. */
    std::atomic<std::uint32_t> ran{0};
    constexpr std::uint32_t COUNT = 4096U + 512U;
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        const JobIndex handle = jobs->submitSingle(
            [](void* context) { static_cast<std::atomic<std::uint32_t>*>(context)->fetch_add(1); },
            &ran, 0, 0, JobPriorityNormal, resonate::JobAffinityThroughput);
        REQUIRE(handle != 0);
    }

    jobs->waitAll();
    REQUIRE(ran.load() == COUNT);

    delete jobs;
}

namespace
{

/* Waits for a flag the way a check can afford to: bounded, so a submission that
   wrongly depends on its own ancestor fails the check instead of hanging the
   suite. */
bool spinUntil(const std::atomic<bool>& flag, int spins)
{
    for (int spin = 0; spin < spins; ++spin)
    {
        if (flag.load(std::memory_order_acquire))
        {
            return true;
        }
        resonate_pal_thread_yield();
    }
    return flag.load(std::memory_order_acquire);
}

void setFlag(void* context)
{
    static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_release);
}

} // namespace

TEST_CASE("nested work does not wait for the job that submitted it", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    struct State
    {
        JobSystem* jobs = nullptr;
        std::atomic<bool> innerRan{false};
        std::atomic<bool> innerRanWhileParentRuns{false};
        std::atomic<bool> innerWaited{false};
    } state;
    state.jobs = jobs;

    /* The nested job writes the same group as the task that submits it: without
       the exemption it would depend on that task, which is waiting for it. A
       submission the pool refused (handle 0) leaves the flags clear, so the
       checks below still fail rather than pass silently. */
    const JobIndex outer = jobs->submitSingle(
        [](void* context)
        {
            auto* state = static_cast<State*>(context);
            const JobIndex inner =
                state->jobs->submitSingle(setFlag, &state->innerRan, GROUP_A, GROUP_A,
                                          JobPriorityNormal, resonate::JobAffinityThroughput);
            const bool ran = spinUntil(state->innerRan, 20000000);
            state->innerRanWhileParentRuns.store(ran, std::memory_order_release);
            if (ran)
            {
                state->jobs->wait(inner);
                state->innerWaited.store(true, std::memory_order_release);
            }
        },
        &state, 0, GROUP_A, JobPriorityNormal, resonate::JobAffinityThroughput);
    REQUIRE(outer != 0);

    jobs->wait(outer);
    REQUIRE(state.innerRan.load());
    REQUIRE(state.innerRanWhileParentRuns.load());
    REQUIRE(state.innerWaited.load());

    delete jobs;
}

TEST_CASE("nested work of nested work runs", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    struct State
    {
        JobSystem* jobs = nullptr;
        std::atomic<std::uint32_t> completed{0};
        std::atomic<std::uint32_t> reached{0};
    } state;
    state.jobs = jobs;

    /* Three levels deep on one group, each waiting for the level below it: every
       level is an ancestor of the submission two levels down. */
    const JobIndex outer = jobs->submitSingle(
        [](void* context)
        {
            auto* state = static_cast<State*>(context);
            JobSystem* system = state->jobs;

            const JobIndex middle = system->submitSingle(
                [](void* context2)
                {
                    auto* state = static_cast<State*>(context2);
                    JobSystem* system2 = state->jobs;

                    const JobIndex inner = system2->submitSingle(
                        [](void* context3)
                        {
                            static_cast<State*>(context3)->reached.store(3,
                                                                         std::memory_order_relaxed);
                        },
                        state, GROUP_A, GROUP_A, JobPriorityNormal,
                        resonate::JobAffinityThroughput);
                    system2->wait(inner);
                    state->completed.store(2, std::memory_order_relaxed);
                },
                state, GROUP_A, GROUP_A, JobPriorityNormal, resonate::JobAffinityThroughput);
            system->wait(middle);
            state->completed.store(1, std::memory_order_relaxed);
        },
        &state, GROUP_A, GROUP_A, JobPriorityNormal, resonate::JobAffinityThroughput);
    REQUIRE(outer != 0);

    jobs->wait(outer);
    REQUIRE(state.reached.load() == 3);
    REQUIRE(state.completed.load() == 1);

    delete jobs;
}

TEST_CASE("work ordered behind a task runs after the task's nested work", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    struct State
    {
        JobSystem* jobs = nullptr;
        std::atomic<bool> nestedRan{false};
        std::atomic<std::uint32_t> rank{0};
        std::atomic<std::uint32_t> nestedRank{0};
        std::atomic<std::uint32_t> parentRank{0};
        std::atomic<std::uint32_t> followerRank{0};
    } state;
    state.jobs = jobs;

    /* Two jobs on GROUP_A: the second waits for the first, which nests (and
       waits for) its own work. That nested work belongs to the first job, so the
       rank it stamps must land before the rank the second job stamps — which is
       also what keeps the submission finite. */
    const JobIndex parent = jobs->submitSingle(
        [](void* context)
        {
            auto* state = static_cast<State*>(context);
            const JobIndex inner = state->jobs->submitSingle(
                [](void* context2)
                {
                    auto* state2 = static_cast<State*>(context2);
                    state2->nestedRank.store(state2->rank.fetch_add(1, std::memory_order_relaxed),
                                             std::memory_order_relaxed);
                    state2->nestedRan.store(true, std::memory_order_release);
                },
                state, 0, 0, JobPriorityNormal, resonate::JobAffinityThroughput);
            state->jobs->wait(inner);
            state->parentRank.store(state->rank.fetch_add(1, std::memory_order_relaxed),
                                    std::memory_order_relaxed);
        },
        &state, 0, GROUP_A, JobPriorityNormal, resonate::JobAffinityThroughput);
    REQUIRE(parent != 0);

    const JobIndex follower = jobs->submitSingle(
        [](void* context)
        {
            auto* state = static_cast<State*>(context);
            state->followerRank.store(state->rank.fetch_add(1, std::memory_order_relaxed),
                                      std::memory_order_relaxed);
        },
        &state, 0, GROUP_A, JobPriorityNormal, resonate::JobAffinityThroughput);
    REQUIRE(follower != 0);

    jobs->waitAll();
    REQUIRE(state.nestedRan.load());
    REQUIRE(state.nestedRank.load() < state.parentRank.load());
    REQUIRE(state.parentRank.load() < state.followerRank.load());

    delete jobs;
}

TEST_CASE("nested submissions drain from many threads", "[core][job]")
{
    JobSystem* jobs = resonate::createJobSystem();

    struct State
    {
        JobSystem* jobs = nullptr;
        std::atomic<std::uint32_t> slices{0};
        std::atomic<std::uint32_t> parents{0};
    } state;
    state.jobs = jobs;

    /* Eight threads each submit a task that nests a parallel for on the same
       group as itself: every task is its own nested work's ancestor, and the
       tasks serialise against each other, so the ancestry, not the masks, is
       what has to keep every wait finite. */
    const auto parentTask = [](void* context)
    {
        auto* state = static_cast<State*>(context);
        const JobIndex forked = state->jobs->submitParallel(
            [](void* context2, std::uint32_t begin, std::uint32_t end)
            {
                auto* state2 = static_cast<State*>(context2);
                state2->slices.fetch_add(end - begin, std::memory_order_relaxed);
            },
            state, 64, GROUP_C, GROUP_C, JobPriorityNormal);
        state->jobs->wait(forked);
        state->parents.fetch_add(1, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int index = 0; index < 8; ++index)
    {
        threads.emplace_back(
            [&state, parentTask]
            {
                const JobIndex outer =
                    state.jobs->submitSingle(parentTask, &state, 0, GROUP_C, JobPriorityNormal,
                                             resonate::JobAffinityThroughput);
                state.jobs->wait(outer);
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    jobs->waitAll();
    REQUIRE(state.slices.load() == 8 * 64);
    REQUIRE(state.parents.load() == 8);

    delete jobs;
}
