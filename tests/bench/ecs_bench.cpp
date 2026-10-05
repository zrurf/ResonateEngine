#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/core/job.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/pal/sync.h>

struct Position
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Velocity
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

namespace resonate::ecs
{
RESONATE_COMPONENT(Position, "bench.Position");
RESONATE_COMPONENT(Velocity, "bench.Velocity");
} // namespace resonate::ecs

namespace
{

using resonate::ecs::ChunkView;
using resonate::ecs::CommandBuffer;
using resonate::ecs::ComponentIndex;
using resonate::ecs::Entity;
using resonate::ecs::Query;
using resonate::ecs::QueryDesc;
using resonate::ecs::World;

constexpr std::uint32_t COUNT = 1000000;
constexpr std::uint32_t PLAYBACK = 100000;

double millisecondsSince(const std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * 1e3;
}

/* The iteration bar compares against a same-shape scan over raw arrays, not
   against memcpy: the same 12 MB memcpy sits on cache thresholds and drifts
   with machine state, while the raw scan is the same bytes with no storage in
   the way. The bar is 65: chunk-granular SoA turns the scan into ~2500 short
   streams whose prefetcher restarts cost a stable ~25-30% against one long
   scan (observed 70-79 across gcc/Linux and clang-cl/Windows; architecture.md,
   "The ECS bench"), so the gate sits under the floor with room for machine
   variance and still trips on any real storage regression.
*/
constexpr double kBandwidthBar = 0.65;
constexpr double kCreateDestroyGate = 5.0e6; // ops/s tripwire
constexpr double kPlaybackBarMs = 8.0;       // 100k add+remove commands

int gFailures = 0;

void reportBar(const char* name, const double value, const double bar, const bool pass,
               const char* unit)
{
    std::printf("[ecs-bench] %-34s %14.4g %-7s bar %.4g  %s\n", name, value, unit, bar,
                pass ? "PASS" : "FAIL");
    if (!pass)
    {
        ++gFailures;
    }
}

struct Sum
{
    ComponentIndex position = resonate::ecs::kInvalidComponent;
    double total = 0.0;

    static void run(void* context, ChunkView view)
    {
        auto* self = static_cast<Sum*>(context);
        const auto* positions = static_cast<const Position*>(view.componentData(self->position));
        for (std::uint32_t row = 0; row < view.count; ++row)
        {
            self->total += positions[row].x;
        }
    }
};

struct Integrate
{
    ComponentIndex position = resonate::ecs::kInvalidComponent;
    ComponentIndex velocity = resonate::ecs::kInvalidComponent;
    std::uint64_t visited = 0;
    int chunks = 0;

    static void run(void* context, ChunkView view)
    {
        auto* self = static_cast<Integrate*>(context);
        auto* positions = static_cast<Position*>(view.componentData(self->position));
        const auto* velocities = static_cast<const Velocity*>(view.componentData(self->velocity));
        for (std::uint32_t row = 0; row < view.count; ++row)
        {
            positions[row].x += velocities[row].x;
        }
        self->visited += view.count;
        ++self->chunks;
    }
};

struct ParallelCount
{
    std::atomic<std::uint64_t> visited{0};

    static void run(void* context, ChunkView view)
    {
        static_cast<ParallelCount*>(context)->visited.fetch_add(view.count,
                                                                std::memory_order_relaxed);
    }
};

} // namespace

int main()
{
    std::printf("[ecs-bench] hardware threads: %u, iteration entities: %u\n",
                resonate_pal_sync_hardware_concurrency(), COUNT);

    /* create + destroy. Best of three reps; the printed number is the
       acceptance evidence, the gate only trips on a catastrophic regression
       (clang-cl/Windows idles within 10% of the design bar, so gating at the
       bar itself would be a coin flip). */
    double createDestroyOpsPerSecond = 0.0;
    {
        for (int rep = 0; rep < 3; ++rep)
        {
            World world(resonate::systemAllocator());
            std::vector<Entity> entities(COUNT);
            const auto start = std::chrono::steady_clock::now();
            for (std::uint32_t index = 0; index < COUNT; ++index)
            {
                entities[index] = world.create();
            }
            for (std::uint32_t index = 0; index < COUNT; ++index)
            {
                world.destroy(entities[index]);
            }
            const double seconds = millisecondsSince(start) * 1e-3;
            createDestroyOpsPerSecond = std::max(createDestroyOpsPerSecond, 2.0 * COUNT / seconds);
        }
    }
    reportBar("create+destroy tripwire", createDestroyOpsPerSecond / 1e6, kCreateDestroyGate / 1e6,
              createDestroyOpsPerSecond >= kCreateDestroyGate, "M ops/s");

    World dense(resonate::systemAllocator());
    const ComponentIndex position = dense.registerComponent<Position>();
    const ComponentIndex velocity = dense.registerComponent<Velocity>();
    if (position == resonate::ecs::kInvalidComponent ||
        velocity == resonate::ecs::kInvalidComponent)
    {
        std::printf("[ecs-bench] component registration failed\n");
        return 1;
    }

    std::vector<Entity> entities(COUNT);
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        entities[index] = dense.create();
        dense.add(entities[index], Position{static_cast<float>(index), 0.0F, 0.0F});
    }

    /* one component, sequential then parallel: printed for the record */
    {
        const ComponentIndex all[] = {position};
        QueryDesc desc;
        desc.all = resonate::Span<const ComponentIndex>(all, 1);
        Query query = dense.createQuery(desc);

        Sum sum{position};
        const auto start = std::chrono::steady_clock::now();
        query.forEachChunk(&Sum::run, &sum);
        const double ms = millisecondsSince(start);
        if (sum.total != static_cast<double>(COUNT) * (COUNT - 1U) / 2.0)
        {
            std::printf("[ecs-bench] single-component iteration sum mismatch\n");
            return 1;
        }
        std::printf("[ecs-bench] iterate 1 component      %10.2f ns/entity  %6.2f G/s\n",
                    ms * 1e6 / COUNT, COUNT / (ms * 1e-3) / 1e9);

        resonate::JobSystem* jobs = resonate::createJobSystem();
        if (jobs == nullptr)
        {
            std::printf("[ecs-bench] job system creation failed\n");
            return 1;
        }
        ParallelCount counter;
        const auto parallelStart = std::chrono::steady_clock::now();
        query.parallelEachChunk(*jobs, 0, 0, &ParallelCount::run, &counter);
        const double parallelMs = millisecondsSince(parallelStart);
        delete jobs;
        if (counter.visited.load() != COUNT)
        {
            std::printf("[ecs-bench] parallel iteration coverage mismatch\n");
            return 1;
        }
        std::printf("[ecs-bench] iterate 1 component, par %10.2f ns/entity  %6.2f G/s\n",
                    parallelMs * 1e6 / COUNT, COUNT / (parallelMs * 1e-3) / 1e9);
    }

    /* two components: the bandwidth bar against a same-shape scan; memcpy is
       printed for the record */
    double bandwidthRatio = 0.0;
    {
        for (const Entity entity : entities)
        {
            dense.add(entity, Velocity{0.5F, 0.0F, 0.0F});
        }

        const ComponentIndex both[] = {position, velocity};
        QueryDesc desc;
        desc.all = resonate::Span<const ComponentIndex>(both, 2);
        Query query = dense.createQuery(desc);

        /* The same read-2/write-1 scan with no storage in the way. Five
           alternating reps, each side keeping its best: both sides touch 36
           bytes per entity, so the bandwidth ratio is the time ratio. Single
           shots of a 12 MB working set wobble with machine state, and a ratio
           of two wobbly shots wobbles worse. */
        std::vector<Position> rawPositions(COUNT, Position{1.0F, 0.0F, 0.0F});
        std::vector<Velocity> rawVelocities(COUNT, Velocity{0.5F, 0.0F, 0.0F});

        double iterateMs = 1e300;
        double rawMs = 1e300;
        int iterateChunks = 0;
        for (int rep = 0; rep < 5; ++rep)
        {
            Integrate integrate{position, velocity, 0, 0};
            const auto queryStart = std::chrono::steady_clock::now();
            query.forEachChunk(&Integrate::run, &integrate);
            const double queryMs = millisecondsSince(queryStart);
            if (integrate.visited != COUNT)
            {
                std::printf("[ecs-bench] two-component iteration coverage mismatch\n");
                return 1;
            }
            iterateMs = std::min(iterateMs, queryMs);
            iterateChunks = integrate.chunks;

            const auto rawStart = std::chrono::steady_clock::now();
            for (std::uint32_t index = 0; index < COUNT; ++index)
            {
                rawPositions[index].x += rawVelocities[index].x;
            }
            const double scanMs = millisecondsSince(rawStart);
            if (rawPositions[COUNT / 2].x <= 1.0F)
            {
                std::printf("[ecs-bench] raw scan result mismatch\n");
                return 1;
            }
            rawMs = std::min(rawMs, scanMs);
        }
        std::printf(
            "[ecs-bench] iterate 2 components     %10.2f ns/entity  %6.2f G/s  (%d chunks)\n",
            iterateMs * 1e6 / COUNT, COUNT / (iterateMs * 1e-3) / 1e9, iterateChunks);
        std::printf("[ecs-bench] raw scan, same bytes     %10.2f ns/entity  %6.2f G/s\n",
                    rawMs * 1e6 / COUNT, COUNT / (rawMs * 1e-3) / 1e9);

        std::vector<Position> source(COUNT, Position{1.0F, 0.0F, 0.0F});
        std::vector<Position> target(COUNT);
        const auto memcpyStart = std::chrono::steady_clock::now();
        std::memcpy(target.data(), source.data(), sizeof(Position) * COUNT);
        const double memcpyMs = millisecondsSince(memcpyStart);
        std::printf("[ecs-bench] memcpy baseline (12 MB)  %10.2f ns/entity  %6.2f G/s\n",
                    memcpyMs * 1e6 / COUNT, COUNT / (memcpyMs * 1e-3) / 1e9);

        bandwidthRatio = rawMs / iterateMs;
        reportBar("iteration vs same-shape scan", bandwidthRatio * 100.0, kBandwidthBar * 100.0,
                  bandwidthRatio >= kBandwidthBar, "%");
    }

    /* 100k recorded structural commands at a sync point, the playback bar */
    {
        World world(resonate::systemAllocator());
        const ComponentIndex playbackPosition = world.registerComponent<Position>();
        const ComponentIndex playbackVelocity = world.registerComponent<Velocity>();
        std::vector<Entity> playbackEntities(PLAYBACK);
        for (std::uint32_t index = 0; index < PLAYBACK; ++index)
        {
            playbackEntities[index] = world.create();
            world.add(playbackEntities[index], Position{1.0F, 0.0F, 0.0F});
        }

        CommandBuffer buffer(world);
        for (std::uint32_t index = 0; index < PLAYBACK; ++index)
        {
            buffer.add(playbackEntities[index], Velocity{1.0F, 0.0F, 0.0F});
            buffer.remove(playbackEntities[index], playbackPosition);
        }
        if (buffer.commandCount() != 2U * PLAYBACK)
        {
            std::printf("[ecs-bench] command recording mismatch\n");
            return 1;
        }

        const auto start = std::chrono::steady_clock::now();
        world.play(buffer);
        const double ms = millisecondsSince(start);
        reportBar("play 100k add+remove (ms)", ms, kPlaybackBarMs, ms < kPlaybackBarMs, "ms");

        double sum = 0.0;
        for (std::uint32_t index = 0; index < PLAYBACK; ++index)
        {
            const Velocity* value = world.get<Velocity>(playbackEntities[index]);
            if (value == nullptr || world.get<Position>(playbackEntities[index]) != nullptr)
            {
                std::printf("[ecs-bench] playback result mismatch\n");
                return 1;
            }
            sum += value->x;
        }
        if (sum != static_cast<double>(PLAYBACK))
        {
            std::printf("[ecs-bench] playback result mismatch\n");
            return 1;
        }
    }

    if (gFailures != 0)
    {
        std::printf("[ecs-bench] %d bar(s) missed\n", gFailures);
        return 1;
    }
    std::printf("[ecs-bench] all bars met\n");
    return 0;
}
