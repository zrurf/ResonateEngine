#include <catch2/catch_all.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <resonate/application/application.h>
#include <resonate/application/systems.h>
#include <resonate/core/allocator.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/module/host.hpp>
#include <resonate/render/offscreen.hpp>

#include "paths.h"

/*
 * The frame's sync points: what a system records is played between UPDATE and
 * PHYSICS and between PHYSICS and LATE, so structural changes are visible to
 * the same frame's later stages, once.
 */

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

struct Reports
{
    std::vector<std::string> lines;

    static void sink(void* user_data, World::Report, const char* message)
    {
        static_cast<Reports*>(user_data)->lines.emplace_back(message != nullptr ? message : "");
    }

    [[nodiscard]] bool contains(const char* fragment) const
    {
        for (const std::string& line : lines)
        {
            if (line.find(fragment) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
};

struct Logs
{
    std::vector<std::string> lines;

    static void sink(void* user_data, int32_t, const char*, int32_t, const char* message)
    {
        static_cast<Logs*>(user_data)->lines.emplace_back(message != nullptr ? message : "");
    }

    [[nodiscard]] bool contains(const char* fragment) const
    {
        for (const std::string& line : lines)
        {
            if (line.find(fragment) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
};

/* Records one spawn into the buffer it is handed, whatever stage it runs in. */
struct Spawner
{
    Entity spawned{};
    int runs = 0;
    bool spawn_failed = false;

    static void run(void* context, World&, CommandBuffer& commands, float)
    {
        auto* self = static_cast<Spawner*>(context);
        ++self->runs;

        self->spawned = commands.spawn(Marker{1U});
        if (!self->spawned.valid())
        {
            self->spawn_failed = true;
        }
    }
};

void countChunks(void* context, ChunkView view)
{
    *static_cast<std::uint32_t*>(context) += view.count;
}

std::uint32_t markerCount(Query& query)
{
    std::uint32_t count = 0;
    query.forEachChunk(&countChunks, &count);
    return count;
}

} // namespace

namespace resonate::ecs
{

RESONATE_COMPONENT(Marker, "rtest.Marker");

} // namespace resonate::ecs

TEST_CASE("the sync point plays what a system recorded", "[runtime][frame]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Marker>() != resonate::ecs::kInvalidComponent);

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    host->setWorld(&world);

    Spawner spawner;
    const resonate::EngineSystemDesc desc{"test.spawner", RESONATE_STAGE_UPDATE, &Spawner::run,
                                          &spawner};
    REQUIRE(resonate::addEngineSystem(*host, desc) == RESONATE_OK);

    host->runStage(RESONATE_STAGE_UPDATE, 1.0F / 60.0F);
    REQUIRE(spawner.runs == 1);

    /* Read-committed: recorded, not existent. */
    REQUIRE_FALSE(spawner.spawn_failed);
    REQUIRE_FALSE(world.alive(spawner.spawned));
    REQUIRE(world.entityCount() == 0U);

    host->playRecordedCommands();
    REQUIRE(world.alive(spawner.spawned));
    REQUIRE(world.entityCount() == 1U);

    /* A buffer that played is empty, so a second sync point plays nothing. */
    host->playRecordedCommands();
    REQUIRE(world.entityCount() == 1U);
}

TEST_CASE("playback is refused while parallel execution is in flight", "[runtime][frame]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Marker>() != resonate::ecs::kInvalidComponent);

    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    host->setWorld(&world);

    Spawner spawner;
    const resonate::EngineSystemDesc desc{"test.spawner", RESONATE_STAGE_UPDATE, &Spawner::run,
                                          &spawner};
    REQUIRE(resonate::addEngineSystem(*host, desc) == RESONATE_OK);

    host->runStage(RESONATE_STAGE_UPDATE, 1.0F / 60.0F);
    REQUIRE(spawner.runs == 1);

    /* The guard World::play already keeps, reused by the sync point: refused as
       a whole, and the buffer keeps its commands. */
    {
        World::ParallelScope parallel(world);
        host->playRecordedCommands();

        REQUIRE_FALSE(world.alive(spawner.spawned));
        REQUIRE(world.entityCount() == 0U);
        REQUIRE(reports.contains("parallel execution is in flight"));
    }

    /* Once the scope closes, the next sync point plays it. */
    host->playRecordedCommands();
    REQUIRE(world.alive(spawner.spawned));
    REQUIRE(world.entityCount() == 1U);
}

TEST_CASE("a world-aware system is refused when the run has no world", "[runtime][frame]")
{
    /* Declared before the host: a host reports through the sink while it is
       destroyed, so the sink has to outlive it. */
    Logs logs;

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    host->setLogSink(&logs, &Logs::sink);

    Spawner spawner;
    const resonate::EngineSystemDesc desc{"test.spawner", RESONATE_STAGE_UPDATE, &Spawner::run,
                                          &spawner};
    REQUIRE(resonate::addEngineSystem(*host, desc) == RESONATE_E_STATE);
    REQUIRE(logs.contains("has no world"));

    host->runFrame(1.0F / 60.0F);
    REQUIRE(spawner.runs == 0);
}

TEST_CASE("commands recorded after the last sync point are dropped and reported",
          "[runtime][frame]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Marker>() != resonate::ecs::kInvalidComponent);

    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    Logs logs;

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    host->setWorld(&world);
    host->setLogSink(&logs, &Logs::sink);

    /* LATE runs after both sync points, so nothing this frame plays it. */
    Spawner spawner;
    const resonate::EngineSystemDesc desc{"test.late", RESONATE_STAGE_LATE_UPDATE, &Spawner::run,
                                          &spawner};
    REQUIRE(resonate::addEngineSystem(*host, desc) == RESONATE_OK);

    host->runFrame(1.0F / 60.0F);

    REQUIRE(spawner.runs == 1);
    REQUIRE_FALSE(world.alive(spawner.spawned));
    REQUIRE(world.entityCount() == 0U);
    REQUIRE(logs.contains("after the last sync point"));
}

TEST_CASE("an unplayed buffer is reclaimed and reported when the host goes away",
          "[runtime][frame]")
{
    /* Declared before the host: the host's buffers release through the world,
       which is the ordering Application::run keeps too. */
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Marker>() != resonate::ecs::kInvalidComponent);

    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    host->setWorld(&world);

    Spawner spawner;
    const resonate::EngineSystemDesc desc{"test.late", RESONATE_STAGE_LATE_UPDATE, &Spawner::run,
                                          &spawner};
    REQUIRE(resonate::addEngineSystem(*host, desc) == RESONATE_OK);

    /* A frame that never reaches a sync point after the recording: the buffer
       still holds its create when the host is destroyed. */
    host->runStage(RESONATE_STAGE_LATE_UPDATE, 1.0F / 60.0F);
    REQUIRE(spawner.runs == 1);
    REQUIRE_FALSE(world.alive(spawner.spawned));

    host.reset();

    REQUIRE(world.entityCount() == 0U);
    REQUIRE(reports.contains("were never played"));
}

namespace
{

/* Everything one headless run reads back. The system descriptors must outlive
   the registration, so they live here rather than in the setup lambda. */
struct FrameRun
{
    ComponentIndex marker = resonate::ecs::kInvalidComponent;
    Query markers;

    resonate::EngineSystemDesc update_desc;
    resonate::EngineSystemDesc physics_desc;

    int update_runs = 0;
    int physics_runs = 0;
    int frame_one_physics_count = -1;
    int last_physics_count = -1;
    int seen_by_update_before_playback = -1;
    int announced = 0;
    bool spawn_failed = false;
    std::vector<std::string> reports;

    static void report(void* user_data, World::Report, const char* message)
    {
        static_cast<FrameRun*>(user_data)->reports.emplace_back(message != nullptr ? message : "");
    }

    static void announce(void* user_data, World&, Entity, ComponentIndex)
    {
        ++static_cast<FrameRun*>(user_data)->announced;
    }

    /* Records once, and reads the entity count before playback: what this
       frame recorded must not be visible yet. */
    static void update(void* context, World& world, CommandBuffer& commands, float)
    {
        auto* self = static_cast<FrameRun*>(context);
        if (self->update_runs == 0)
        {
            const Entity spawned = commands.spawn(Marker{5U});
            if (!spawned.valid())
            {
                self->spawn_failed = true;
            }
            self->seen_by_update_before_playback = static_cast<int>(world.entityCount());
        }
        ++self->update_runs;
    }

    static void physics(void* context, World&, CommandBuffer&, float)
    {
        auto* self = static_cast<FrameRun*>(context);
        ++self->physics_runs;

        const int count = static_cast<int>(markerCount(self->markers));
        if (self->physics_runs == 1)
        {
            self->frame_one_physics_count = count;
        }
        self->last_physics_count = count;
    }
};

} // namespace

TEST_CASE("a headless run shows UPDATE's spawn to PHYSICS in the same frame", "[runtime][frame]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    INFO("the plugin targets are a build dependency of this test target");
    REQUIRE_FALSE(plugin_directory.empty());

    FrameRun run;

    resonate::Application::Config config;
    config.plugin_directory = plugin_directory;

    /* Offscreen, so the modules that draw attach without a window. */
    config.publish = [](ResonateCapabilityRegistry* registry)
    { return resonate::render::publishOffscreen(registry, 320U, 200U); };

    config.systems = [&run](resonate::ModuleHost& host, World& world)
    {
        world.setReportSink(&run, &FrameRun::report);

        run.marker = world.registerComponent<Marker>();
        if (run.marker == resonate::ecs::kInvalidComponent ||
            !world.addObserver(run.marker, &run, &FrameRun::announce))
        {
            return RESONATE_E_INTERNAL;
        }

        const ComponentIndex all[] = {run.marker};
        QueryDesc desc;
        desc.all = resonate::Span<const ComponentIndex>(all, 1);
        run.markers = world.createQuery(desc);
        if (!run.markers.valid())
        {
            return RESONATE_E_INTERNAL;
        }

        run.update_desc = {"test.spawner", RESONATE_STAGE_UPDATE, &FrameRun::update, &run};
        run.physics_desc = {"test.spotter", RESONATE_STAGE_PHYSICS, &FrameRun::physics, &run};
        if (resonate::addEngineSystem(host, run.update_desc) != RESONATE_OK ||
            resonate::addEngineSystem(host, run.physics_desc) != RESONATE_OK)
        {
            return RESONATE_E_INTERNAL;
        }
        return RESONATE_OK;
    };

    constexpr int FRAMES = 3;
    int frames = 0;
    config.tick = [&frames]
    {
        if (frames >= FRAMES)
        {
            return false;
        }
        ++frames;
        return true;
    };

    REQUIRE(resonate::Application::run(config) == EXIT_SUCCESS);

    REQUIRE(run.update_runs == FRAMES);
    REQUIRE(run.physics_runs == FRAMES);
    REQUIRE_FALSE(run.spawn_failed);

    /* Recorded, invisible to the query of the frame that recorded it. */
    REQUIRE(run.seen_by_update_before_playback == 0);

    /* Visible to PHYSICS in that same frame, and still exactly one entity on
       every frame after it: the buffer played once and did not replay. */
    REQUIRE(run.frame_one_physics_count == 1);
    REQUIRE(run.last_physics_count == 1);

    /* The sync point announces what playback applied; a replay would have
       announced a second arrival. */
    REQUIRE(run.announced == 1);
    REQUIRE(run.reports.empty());
}
