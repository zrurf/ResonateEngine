#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>

namespace
{

using resonate::ecs::ChunkView;
using resonate::ecs::CommandBuffer;
using resonate::ecs::ComponentIndex;
using resonate::ecs::Entity;
using resonate::ecs::Query;
using resonate::ecs::QueryDesc;
using resonate::ecs::World;

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

struct Health
{
    std::int32_t value = 0;
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

QueryDesc describeAll(const ComponentIndex* all, std::size_t count)
{
    QueryDesc desc;
    desc.all = resonate::Span<const ComponentIndex>(all, count);
    return desc;
}

} // namespace

namespace resonate::ecs
{

RESONATE_COMPONENT(Position, "ctest.Position");
RESONATE_COMPONENT(Velocity, "ctest.Velocity");
RESONATE_COMPONENT(Health, "ctest.Health");

} // namespace resonate::ecs

TEST_CASE("a recorded create is not alive until playback", "[ecs][command]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));

    CommandBuffer buffer(world);
    const Entity entity = buffer.spawn(Position{1.0F, 2.0F, 3.0F});
    REQUIRE(entity.valid());
    REQUIRE_FALSE(world.alive(entity));
    REQUIRE(world.entityCount() == 0U);

    /* Read-committed: an unplayed buffer is not visible to queries. */
    REQUIRE(query.matchedChunkCount() == 0U);

    world.play(buffer);
    REQUIRE(world.alive(entity));
    REQUIRE(world.entityCount() == 1U);
    REQUIRE(world.get<Position>(entity) != nullptr);
    REQUIRE(world.get<Position>(entity)->y == 2.0F);
    REQUIRE(query.matchedChunkCount() == 1U);
    REQUIRE(buffer.empty());
}

TEST_CASE("commands apply in record order", "[ecs][command]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);
    const ComponentIndex velocity = world.registerComponent<Velocity>();
    REQUIRE(velocity != resonate::ecs::kInvalidComponent);
    const ComponentIndex health = world.registerComponent<Health>();
    REQUIRE(health != resonate::ecs::kInvalidComponent);

    CommandBuffer buffer(world);
    const Entity kept = buffer.create();
    buffer.add(kept, Position{1.0F, 1.0F, 1.0F});
    buffer.add(kept, Velocity{2.0F, 2.0F, 2.0F});
    buffer.remove(kept, position);

    /* Created and destroyed inside one buffer: both commands run in order. */
    const Entity gone = buffer.create();
    buffer.add(gone, Health{5});
    buffer.destroy(gone);
    REQUIRE(buffer.commandCount() == 7U);

    world.play(buffer);
    REQUIRE(world.alive(kept));
    REQUIRE(world.get<Velocity>(kept) != nullptr);
    REQUIRE(world.get<Position>(kept) == nullptr);
    REQUIRE_FALSE(world.alive(gone));
    REQUIRE(world.entityCount() == 1U);
    REQUIRE(buffer.empty());
}

TEST_CASE("recording refuses what the world does not know", "[ecs][command]")
{
    Reports reports;
    World world(resonate::systemAllocator());
    world.setReportSink(&reports, &Reports::sink);
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    CommandBuffer buffer(world);

    /* A handle whose entity is gone. */
    const Entity stale = world.create();
    world.destroy(stale);
    buffer.add(stale, Position{1.0F, 0.0F, 0.0F});
    buffer.remove(stale, position);
    buffer.destroy(stale);
    REQUIRE(buffer.commandCount() == 0U);
    REQUIRE(reports.contains("neither alive nor created"));

    /* Registered, but in another world: this one does not know it. */
    {
        World other(resonate::systemAllocator());
        REQUIRE(other.registerComponent<Velocity>() != resonate::ecs::kInvalidComponent);
    }
    const Entity live = world.create();
    buffer.add(live, Velocity{1.0F, 0.0F, 0.0F});
    REQUIRE(buffer.commandCount() == 0U);
    REQUIRE(reports.contains("not registered in this world"));

    /* An entity this buffer created is a legal target. */
    buffer.add(live, Position{1.0F, 0.0F, 0.0F});
    const Entity fresh = buffer.create();
    buffer.add(fresh, Position{2.0F, 0.0F, 0.0F});
    REQUIRE(buffer.commandCount() == 3U);
}

TEST_CASE("a buffer discarded with unplayed creates releases them", "[ecs][command]")
{
    Reports reports;
    World world(resonate::systemAllocator());
    world.setReportSink(&reports, &Reports::sink);

    Entity reserved{};
    {
        CommandBuffer buffer(world);
        reserved = buffer.create();
        REQUIRE(reserved.valid());
        REQUIRE_FALSE(world.alive(reserved));
        REQUIRE(world.entityCount() == 0U);
    }

    REQUIRE(reports.contains("never played"));
    REQUIRE(world.entityCount() == 0U);

    /* The index is back in the free list, with a fresh generation. */
    const Entity reused = world.create();
    REQUIRE(reused.index == reserved.index);
    REQUIRE(reused.generation != reserved.generation);
    REQUIRE(world.alive(reused));
}

TEST_CASE("play is refused while parallel execution is in flight", "[ecs][command]")
{
    Reports reports;
    World world(resonate::systemAllocator());
    world.setReportSink(&reports, &Reports::sink);
    REQUIRE(world.registerComponent<Position>() != resonate::ecs::kInvalidComponent);

    CommandBuffer buffer(world);
    const Entity entity = buffer.spawn(Position{1.0F, 0.0F, 0.0F});

    {
        const World::ParallelScope parallel(world);
        world.play(buffer);
        REQUIRE_FALSE(world.alive(entity));
        REQUIRE(buffer.commandCount() == 2U);
        REQUIRE(reports.contains("parallel execution is in flight"));
    }

    world.play(buffer);
    REQUIRE(world.alive(entity));
    REQUIRE(world.get<Position>(entity) != nullptr);
}

TEST_CASE("a chunk view goes stale when structure changes", "[ecs][command]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);
    REQUIRE(world.registerComponent<Velocity>() != resonate::ecs::kInvalidComponent);

    const Entity entity = world.create();
    REQUIRE(world.add(entity, Position{1.0F, 0.0F, 0.0F}) != nullptr);

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));

    struct Capture
    {
        ChunkView view{};
        std::uint32_t visits = 0;

        static void run(void* context, ChunkView view)
        {
            auto* self = static_cast<Capture*>(context);
            self->view = view;
            ++self->visits;
        }
    } capture;

    query.forEachChunk(&Capture::run, &capture);
    REQUIRE(capture.visits == 1U);
    REQUIRE(capture.view.valid());

    /* Moving the entity to another archetype changes the chunk it left. */
    CommandBuffer buffer(world);
    buffer.add(entity, Velocity{0.0F, 0.0F, 0.0F});
    world.play(buffer);
    REQUIRE_FALSE(capture.view.valid());

    Capture fresh;
    query.forEachChunk(&Capture::run, &fresh);
    REQUIRE(fresh.view.valid());
}

TEST_CASE("a system may record while it iterates", "[ecs][command]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    constexpr std::uint32_t COUNT = 3000;
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        const Entity entity = world.create();
        REQUIRE(world.add(entity, Position{static_cast<float>(index), 0.0F, 0.0F}) != nullptr);
    }

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));

    struct DestroyAll
    {
        CommandBuffer* buffer = nullptr;
        std::uint32_t visited = 0;

        static void run(void* context, ChunkView view)
        {
            auto* self = static_cast<DestroyAll*>(context);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                self->buffer->destroy(view.entities[row]);
            }
            self->visited += view.count;
        }
    };

    CommandBuffer buffer(world);
    DestroyAll recorder{&buffer};
    query.forEachChunk(&DestroyAll::run, &recorder);

    REQUIRE(recorder.visited == COUNT);
    REQUIRE(buffer.commandCount() == COUNT);
    REQUIRE(world.entityCount() == COUNT);

    world.play(buffer);
    REQUIRE(world.entityCount() == 0U);
}

TEST_CASE("playing one hundred thousand recorded adds", "[ecs][command][benchmark]")
{
    constexpr std::uint32_t COUNT = 100000;

    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    std::vector<Entity> entities(COUNT);
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        entities[index] = world.create();
        REQUIRE(entities[index].valid());
    }

    CommandBuffer buffer(world);
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        buffer.add(entities[index], Position{static_cast<float>(index), 0.0F, 0.0F});
    }
    REQUIRE(buffer.commandCount() == COUNT);

    const auto start = std::chrono::steady_clock::now();
    world.play(buffer);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("[ecs] play %u recorded adds: %.1f ms (%.1f M commands/s)\n", COUNT, seconds * 1e3,
                static_cast<double>(COUNT) / seconds / 1e6);

    double sum = 0.0;
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        const Position* value = world.get<Position>(entities[index]);
        REQUIRE(value != nullptr);
        sum += value->x;
    }
    REQUIRE(sum == static_cast<double>(COUNT) * static_cast<double>(COUNT - 1U) / 2.0);
    REQUIRE(seconds < 1.0);
}
