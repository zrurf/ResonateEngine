#include <catch2/catch_all.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <resonate/application/application.h>
#include <resonate/application/systems.h>
#include <resonate/core/job.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/query.h>
#include <resonate/ecs/world.h>
#include <resonate/module/host.hpp>
#include <resonate/pal/thread.h>
#include <resonate/pal/topology.h>
#include <resonate/render/offscreen.hpp>

#include "paths.h"

namespace
{

using resonate::ecs::ChunkView;
using resonate::ecs::CommandBuffer;
using resonate::ecs::ComponentIndex;
using resonate::ecs::Entity;
using resonate::ecs::Query;
using resonate::ecs::QueryDesc;
using resonate::ecs::World;

struct Marker
{
    std::uint32_t value = 0;
};

void countChunks(void* context, ChunkView view)
{
    *static_cast<std::uint32_t*>(context) += view.count;
}

/* One counting system per stage. The script marks every frame from the tick, so
   what the stages did turns into the run's per-frame cadence and its totals. */
struct StageRuns
{
    int update = 0;
    int physics = 0;
    int render = 0;
    int present = 0;
    float update_delta = 0.0F;
    float render_delta = 0.0F;
    std::vector<std::uint32_t> frame_marks; /* update count seen at each tick */

    void markFrame()
    {
        frame_marks.push_back(static_cast<std::uint32_t>(update));
    }

    /* Steps each frame ran: the first mark is the snapshot before the first
       frame, each following one closes a frame, and the run's total closes the
       last. */
    [[nodiscard]] std::vector<std::uint32_t> stepsPerFrame() const
    {
        std::vector<std::uint32_t> steps;
        for (std::size_t index = 1; index < frame_marks.size(); ++index)
        {
            steps.push_back(frame_marks[index] - frame_marks[index - 1]);
        }
        steps.push_back(static_cast<std::uint32_t>(update) -
                        (frame_marks.empty() ? 0U : frame_marks.back()));
        return steps;
    }

    static void countUpdate(void* context, World&, CommandBuffer&, float delta_seconds)
    {
        auto* self = static_cast<StageRuns*>(context);
        ++self->update;
        self->update_delta = delta_seconds;
    }

    static void countPhysics(void* context, World&, CommandBuffer&, float)
    {
        ++static_cast<StageRuns*>(context)->physics;
    }

    static void countRender(void* context, World&, CommandBuffer&, float delta_seconds)
    {
        auto* self = static_cast<StageRuns*>(context);
        ++self->render;
        self->render_delta = delta_seconds;
    }

    static void countPresent(void* context, World&, CommandBuffer&, float)
    {
        ++static_cast<StageRuns*>(context)->present;
    }
};

/* The counting systems' descriptors, which must outlive the registration. */
struct StageWitness
{
    StageRuns runs;
    resonate::EngineSystemDesc update;
    resonate::EngineSystemDesc physics;
    resonate::EngineSystemDesc render;
    resonate::EngineSystemDesc present;

    ResonateStatus attach(resonate::ModuleHost& host)
    {
        update = {"test.acceptance.update", RESONATE_STAGE_UPDATE, &StageRuns::countUpdate, &runs};
        physics = {"test.acceptance.physics", RESONATE_STAGE_PHYSICS, &StageRuns::countPhysics,
                   &runs};
        render = {"test.acceptance.render", RESONATE_STAGE_RENDER, &StageRuns::countRender, &runs};
        present = {"test.acceptance.present", RESONATE_STAGE_PRESENT, &StageRuns::countPresent,
                   &runs};
        if (resonate::addEngineSystem(host, update) != RESONATE_OK ||
            resonate::addEngineSystem(host, physics) != RESONATE_OK ||
            resonate::addEngineSystem(host, render) != RESONATE_OK ||
            resonate::addEngineSystem(host, present) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        return RESONATE_OK;
    }
};

/* The frame times a run is driven with, one per frame; the run lasts exactly as
   long as the script, and each tick marks the frame it is about to start. */
struct FrameScript
{
    std::vector<float> seconds;
    std::size_t next = 0;

    bool tick(StageRuns& runs)
    {
        if (next >= seconds.size())
        {
            return false;
        }
        runs.markFrame();
        return true;
    }

    float elapsed()
    {
        return seconds[next++];
    }
};

/* A headless run: the offscreen canvas so the drawing module attaches without a
   window, and engine systems for the stages under test. */
resonate::Application::Config headlessConfig(const std::string& plugin_directory)
{
    resonate::Application::Config config;
    config.plugin_directory = plugin_directory;
    config.publish = [](ResonateCapabilityRegistry* registry)
    { return resonate::render::publishOffscreen(registry, 320U, 200U); };
    return config;
}

std::string pluginsDirectory()
{
    /* This target depends on the plugins being built, so a missing directory is
       a build problem rather than a reason to pass without running. */
    const std::string directory = resonate::test::findBuildDirectory("plugins");
    INFO("the plugin targets are a build dependency of this test target");
    REQUIRE_FALSE(directory.empty());
    return directory;
}

} // namespace

namespace resonate::ecs
{

RESONATE_COMPONENT(Marker, "resonate.test.acceptance.Marker");

} // namespace resonate::ecs

TEST_CASE("frame acceptance: a jittery cadence steps the simulation at the fixed rate",
          "[runtime][acceptance]")
{
    FrameScript script;
    script.seconds = {0.005F, 0.005F, 0.01F, 0.04F, 0.0025F};

    StageWitness witness;
    resonate::Application::Config config = headlessConfig(pluginsDirectory());
    config.frame_clock.step_seconds = 0.01F;
    config.frame_elapsed = [&script] { return script.elapsed(); };
    config.tick = [&script, &witness] { return script.tick(witness.runs); };
    config.systems = [&witness](resonate::ModuleHost& host, World&)
    { return witness.attach(host); };

    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);

    /* The scripted frame times turn into whole 10ms steps: the leftovers of one
       frame step in the next, and the 40ms frame runs four. */
    const std::vector<std::uint32_t> expected = {0U, 1U, 1U, 4U, 0U};
    REQUIRE(witness.runs.stepsPerFrame() == expected);

    /* The simulation stages run per step; the draw side once per frame, on the
       frame's own wall time rather than the step. */
    REQUIRE(witness.runs.update == 6);
    REQUIRE(witness.runs.physics == 6);
    REQUIRE(witness.runs.render == 5);
    REQUIRE(witness.runs.present == 5);
    REQUIRE(witness.runs.update_delta == Catch::Approx(0.01F));
    REQUIRE(witness.runs.render_delta == Catch::Approx(0.0025F));
}

TEST_CASE("frame acceptance: a stalled frame is clamped and its debt dropped",
          "[runtime][acceptance]")
{
    /* A third of a second of wall time — a suspended process, a breakpoint hit
       — and then one ordinary frame. */
    FrameScript script;
    script.seconds = {1.0F / 3.0F, 0.02F};

    StageWitness witness;
    resonate::Application::Config config = headlessConfig(pluginsDirectory());
    config.frame_elapsed = [&script] { return script.elapsed(); };
    config.tick = [&script, &witness] { return script.tick(witness.runs); };
    config.systems = [&witness](resonate::ModuleHost& host, World&)
    { return witness.attach(host); };

    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);

    /* The stall owed ~19 steps at 60Hz; the default budget ran five and dropped
       the rest. The next frame ran its own step plus the sub-step remainder
       only — had the dropped steps been carried, it would have run ~16. */
    const std::vector<std::uint32_t> expected = {5U, 2U};
    REQUIRE(witness.runs.stepsPerFrame() == expected);
    REQUIRE(witness.runs.render == 2);
}

TEST_CASE("frame acceptance: a paused simulation still runs the frame", "[runtime][acceptance]")
{
    FrameScript script;
    script.seconds = {0.02F, 0.02F};

    StageWitness witness;
    resonate::Application::Config config = headlessConfig(pluginsDirectory());
    config.frame_clock.paused = true;
    config.frame_elapsed = [&script] { return script.elapsed(); };
    config.tick = [&script, &witness] { return script.tick(witness.runs); };
    config.systems = [&witness](resonate::ModuleHost& host, World&)
    { return witness.attach(host); };

    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);

    /* No simulation, but the frame loop keeps going: what draws keeps drawing,
       which is what makes pause a time-flow parameter and not a stop. */
    REQUIRE(witness.runs.stepsPerFrame() == std::vector<std::uint32_t>{0U, 0U});
    REQUIRE(witness.runs.update == 0);
    REQUIRE(witness.runs.physics == 0);
    REQUIRE(witness.runs.render == 2);
    REQUIRE(witness.runs.present == 2);
}

TEST_CASE("frame acceptance: every simulation step plays its own commands", "[runtime][acceptance]")
{
    /* One 40ms frame at a 10ms step: four simulation steps, each recording one
       entity, each finding what it recorded alive in the stage after the sync
       point. */
    FrameScript script;
    script.seconds = {0.04F};

    struct Playback
    {
        StageRuns runs;
        Query markers;
        std::vector<std::uint32_t> physics_counts;
        std::vector<std::string> host_messages;
        bool spawn_failed = false;
        resonate::EngineSystemDesc update;
        resonate::EngineSystemDesc physics;

        static void sink(void* user_data, int32_t, const char*, int32_t, const char* message)
        {
            static_cast<Playback*>(user_data)->host_messages.emplace_back(
                message != nullptr ? message : "");
        }

        static void record(void* context, World&, CommandBuffer& commands, float)
        {
            auto* self = static_cast<Playback*>(context);
            ++self->runs.update;
            if (!commands.spawn(Marker{1U}).valid())
            {
                self->spawn_failed = true;
            }
        }

        static void count(void* context, World&, CommandBuffer&, float)
        {
            auto* self = static_cast<Playback*>(context);
            std::uint32_t count = 0;
            self->markers.forEachChunk(&countChunks, &count);
            self->physics_counts.push_back(count);
        }
    } playback;

    resonate::Application::Config config = headlessConfig(pluginsDirectory());
    config.frame_clock.step_seconds = 0.01F;
    config.frame_elapsed = [&script] { return script.elapsed(); };
    config.tick = [&script, &playback] { return script.tick(playback.runs); };
    config.systems = [&playback](resonate::ModuleHost& host, World& world)
    {
        host.setLogSink(&playback, &Playback::sink);

        const ComponentIndex marker = world.registerComponent<Marker>();
        if (marker == resonate::ecs::kInvalidComponent)
        {
            return RESONATE_E_INTERNAL;
        }

        const ComponentIndex all[] = {marker};
        QueryDesc desc;
        desc.all = resonate::Span<const ComponentIndex>(all, 1);
        playback.markers = world.createQuery(desc);
        if (!playback.markers.valid())
        {
            return RESONATE_E_INTERNAL;
        }

        playback.update = {"test.acceptance.record", RESONATE_STAGE_UPDATE, &Playback::record,
                           &playback};
        playback.physics = {"test.acceptance.observe", RESONATE_STAGE_PHYSICS, &Playback::count,
                            &playback};
        if (resonate::addEngineSystem(host, playback.update) != RESONATE_OK ||
            resonate::addEngineSystem(host, playback.physics) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        return RESONATE_OK;
    };

    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);

    REQUIRE_FALSE(playback.spawn_failed);
    /* Each step's sync point played what that step recorded: the stage after it
       sees one more entity per step, and the frame's end dropped nothing. */
    REQUIRE(playback.physics_counts == std::vector<std::uint32_t>{1U, 2U, 3U, 4U});
    for (const std::string& message : playback.host_messages)
    {
        INFO(message);
    }
    bool dropped = false;
    for (const std::string& message : playback.host_messages)
    {
        dropped = dropped || message.find("after the last sync point") != std::string::npos;
    }
    REQUIRE_FALSE(dropped);
}

TEST_CASE("frame acceptance: the worker count is retuned at frame boundaries",
          "[runtime][acceptance]")
{
    /* Six one-step frames. The count is shrunk after the first, allowed one
       warm frame, measured, raised again, and measured once the pool is back. */
    constexpr std::size_t FRAMES = 6;
    const std::vector<float> frame_seconds(FRAMES, 0.02F);

    struct PoolWitness
    {
        resonate::JobSystem* jobs = nullptr;
        std::uint32_t capacity = 0;
        std::mutex mutex;
        std::vector<std::set<std::uint32_t>> workers_per_run; /* non-main thread ids */
        std::vector<std::uint32_t> slices_per_run;
        resonate::EngineSystemDesc update;

        static void slice(void* context, std::uint32_t begin, std::uint32_t end)
        {
            auto* self = static_cast<PoolWitness*>(context);
            std::lock_guard<std::mutex> lock(self->mutex);
            self->slices_per_run.back() += end - begin;
            if (resonate_pal_thread_is_main() != 0U)
            {
                return;
            }
            self->workers_per_run.back().insert(resonate_pal_thread_id());
        }

        static void run(void* context, World&, CommandBuffer&, float)
        {
            auto* self = static_cast<PoolWitness*>(context);
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->workers_per_run.emplace_back();
                self->slices_per_run.push_back(0U);
            }
            /* Waited on by handle: this body is itself an outstanding job, so
               "everything has completed" is never true inside it. */
            const resonate::JobIndex job = self->jobs->submitParallel(
                &PoolWitness::slice, self, 64U, 0U, 0U, resonate::JobPriorityNormal);
            self->jobs->wait(job);
        }
    } witness;

    std::size_t next_frame = 0;
    resonate::Application::Config config = headlessConfig(pluginsDirectory());
    /* The step is the frame time, so every frame runs exactly one step and one
       pool run, and the run index is the frame index. */
    config.frame_clock.step_seconds = 0.02F;
    config.frame_elapsed = [&] { return frame_seconds[next_frame - 1U]; };
    config.systems = [&witness](resonate::ModuleHost& host, World&)
    {
        witness.jobs = host.jobs();
        if (witness.jobs == nullptr)
        {
            return RESONATE_E_INTERNAL;
        }
        witness.capacity = witness.jobs->workerCapacity();
        witness.update = {"test.acceptance.pool", RESONATE_STAGE_UPDATE, &PoolWitness::run,
                          &witness};
        return resonate::addEngineSystem(host, witness.update);
    };
    config.tick = [&]
    {
        if (next_frame >= FRAMES)
        {
            return false;
        }
        const std::size_t frame = next_frame++;

        /* A count takes effect at the frame boundary it is requested on: the
           tick runs before that frame's stages. */
        if (frame == 1U)
        {
            witness.jobs->requestWorkerCount(2U);

            /* The awake count settles as workers reach their park. */
            int stable = 0;
            for (int spin = 0; spin < 10000000 && stable < 1000; ++spin)
            {
                stable = witness.jobs->workerCount() <= 2U ? stable + 1 : 0;
            }
            REQUIRE(stable == 1000);
        }
        else if (frame == 4U)
        {
            witness.jobs->requestWorkerCount(witness.capacity);
        }
        return true;
    };

    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);

    /* Every frame ran its step, every step all of its slices: the shrunk pool
       ran the schedule without stalling. */
    REQUIRE(witness.workers_per_run.size() == FRAMES);
    for (const std::uint32_t slices : witness.slices_per_run)
    {
        REQUIRE(slices == 64U);
    }

    /* Frame 2 is the first after the shrink settled; frame 3 is measured, so a
       worker that read the count before the shrink had its warm frame. How many
       workers a batch reaches is service level, so only the ceiling is
       asserted; that the restored pool reaches two at once is proven where it
       can be, in the scene visit below. */
    REQUIRE(witness.workers_per_run[3].size() <= 2U);
}

TEST_CASE("frame acceptance: a 100k-entity step is visited by the pool", "[runtime][acceptance]")
{
    constexpr std::uint32_t ENTITIES = 100000U;
    constexpr std::uint32_t FRAMES = 3U;

    FrameScript script;
    script.seconds = {0.02F, 0.02F, 0.02F};

    struct SceneVisit
    {
        Query markers;
        resonate::JobSystem* jobs = nullptr;
        std::atomic<std::uint64_t> visited{0};
        std::atomic<std::uint32_t> pairing_attempts{0};
        std::atomic<bool> paired{false}; /* two workers ran slices at once */
        std::mutex mutex;
        std::set<std::uint32_t> workers;
        std::set<std::uint32_t> cores;
        resonate::EngineSystemDesc update;

        static void chunk(void* context, ChunkView view)
        {
            auto* self = static_cast<SceneVisit*>(context);
            self->visited.fetch_add(view.count, std::memory_order_relaxed);
            if (resonate_pal_thread_is_main() != 0U)
            {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->workers.insert(resonate_pal_thread_id());
                self->cores.insert(resonate_pal_thread_current_cpu());
                if (self->workers.size() >= 2U)
                {
                    self->paired.store(true, std::memory_order_relaxed);
                    return;
                }
            }

            /* One worker alone can drain a parallel-for before any other wakes,
               so the first arrivals wait a bounded moment for a partner: how
               many workers a pool reaches is service level, but that it reaches
               two at once on a two-core machine is the claim. */
            if (self->pairing_attempts.fetch_add(1U, std::memory_order_relaxed) < 4U)
            {
                for (int spin = 0; spin < 50000 && !self->paired.load(std::memory_order_relaxed);
                     ++spin)
                {
                    resonate_pal_thread_yield();
                }
            }
        }

        static void run(void* context, World&, CommandBuffer&, float)
        {
            auto* self = static_cast<SceneVisit*>(context);
            self->markers.parallelEachChunk(*self->jobs, 0U, 0U, &SceneVisit::chunk, self);
        }
    } visit;

    StageWitness witness;
    resonate::Application::Config config = headlessConfig(pluginsDirectory());
    config.frame_elapsed = [&script] { return script.elapsed(); };
    config.tick = [&script, &witness] { return script.tick(witness.runs); };
    config.systems = [&visit, &witness](resonate::ModuleHost& host, World& world)
    {
        if (witness.attach(host) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }

        const ComponentIndex marker = world.registerComponent<Marker>();
        if (marker == resonate::ecs::kInvalidComponent)
        {
            return RESONATE_E_INTERNAL;
        }
        for (std::uint32_t index = 0; index < ENTITIES; ++index)
        {
            const Entity entity = world.create();
            if (!entity.valid() || world.add(entity, Marker{index}) == nullptr)
            {
                return RESONATE_E_INTERNAL;
            }
        }

        const ComponentIndex all[] = {marker};
        QueryDesc desc;
        desc.all = resonate::Span<const ComponentIndex>(all, 1);
        visit.markers = world.createQuery(desc);
        visit.jobs = host.jobs();
        if (!visit.markers.valid() || visit.jobs == nullptr)
        {
            return RESONATE_E_INTERNAL;
        }

        visit.update = {"test.acceptance.scene", RESONATE_STAGE_UPDATE, &SceneVisit::run, &visit};
        return resonate::addEngineSystem(host, visit.update);
    };

    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);

    /* Three steps, every entity visited in each, the pool's workers took part,
       and every processor they ran on is one the topology placed them on. */
    REQUIRE(witness.runs.update == FRAMES);
    REQUIRE(visit.visited.load() == static_cast<std::uint64_t>(ENTITIES) * FRAMES);

    const ResonatePalTopology* topology = resonate_pal_topology_query();
    std::set<std::uint32_t> placed;
    for (std::uint32_t index = 0; index < topology->core_count; ++index)
    {
        placed.insert(topology->cores[index].id);
    }
    for (const std::uint32_t cpu : visit.cores)
    {
        REQUIRE(placed.count(cpu) == 1U);
    }
    if (topology->core_count >= 2U)
    {
        REQUIRE(visit.paired.load());
    }
}
