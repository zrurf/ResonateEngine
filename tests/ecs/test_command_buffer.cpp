#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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

TEST_CASE("parallel sections record and merge by order key", "[ecs][command]")
{
    World world(resonate::systemAllocator());
    world.registerComponent<Health>();

    CommandBuffer buffer(world);
    std::vector<std::uint64_t> playback_order;

    constexpr std::uint32_t THREADS = 4;
    constexpr std::uint32_t PER_THREAD = 50;
    std::vector<std::vector<Entity>> handles(THREADS);

    std::vector<std::thread> threads;
    for (std::uint32_t key = 0; key < THREADS; ++key)
    {
        threads.emplace_back(
            [&buffer, &playback_order, &handles, key]
            {
                auto section = buffer.section(key);

                for (std::uint32_t index = 0; index < PER_THREAD; ++index)
                {
                    const std::int32_t value = static_cast<std::int32_t>(key * PER_THREAD + index);
                    const Entity entity = section.create();
                    REQUIRE(entity.valid());
                    section.add<Health>(entity, Health{value});
                    handles[key].push_back(entity);
                }

                /* A custom command whose playback appends its key: the merge
                   order becomes observable. */
                const std::uint64_t key_value = key;
                section.record(
                    [](void* context, World&, const void* payload)
                    {
                        auto* order = static_cast<std::vector<std::uint64_t>*>(context);
                        std::uint64_t key = 0;
                        std::memcpy(&key, payload, sizeof(key));
                        order->push_back(key);
                    },
                    &playback_order, &key_value, sizeof(key_value));
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    REQUIRE(buffer.commandCount() == THREADS * (PER_THREAD * 2U + 1U));

    /* Nothing recorded is alive before playback: read-committed holds for
       sections too. */
    for (const std::vector<Entity>& batch : handles)
    {
        for (const Entity entity : batch)
        {
            CHECK_FALSE(world.alive(entity));
        }
    }

    world.play(buffer);

    REQUIRE(world.entityCount() == THREADS * PER_THREAD);
    REQUIRE(playback_order.size() == THREADS);
    for (std::uint32_t index = 0; index < THREADS; ++index)
    {
        CHECK(playback_order[index] == index);
    }

    /* Every section's entities hold exactly the values it recorded. */
    for (std::uint32_t key = 0; key < THREADS; ++key)
    {
        for (std::uint32_t index = 0; index < PER_THREAD; ++index)
        {
            const Health* value = world.get<Health>(handles[key][index]);
            REQUIRE(value != nullptr);
            CHECK(value->value == static_cast<std::int32_t>(key * PER_THREAD + index));
        }
    }
}

TEST_CASE("a section may name what another section created", "[ecs][command]")
{
    World world(resonate::systemAllocator());
    world.registerComponent<Health>();
    world.registerComponent<Velocity>();

    CommandBuffer buffer(world);
    Entity spawned{};
    std::atomic<bool> created{false};

    std::thread creator(
        [&buffer, &spawned, &created]
        {
            auto section = buffer.section(0);
            spawned = section.create();
            REQUIRE(spawned.valid());
            section.add<Health>(spawned, Health{10});
            created.store(true, std::memory_order_release);
        });

    std::thread extender(
        [&buffer, &spawned, &created]
        {
            while (!created.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            auto section = buffer.section(1);
            /* The entity is not alive yet — but the buffer created it, so the
               add is accepted, and by key order it plays after the create. */
            section.add<Velocity>(spawned, Velocity{1.0F, 2.0F, 3.0F});
        });

    creator.join();
    extender.join();

    world.play(buffer);

    /* Both sections' commands applied to the one entity: the create and the
       Health from section 0, the Velocity from section 1. */
    const Health* health = world.get<Health>(spawned);
    REQUIRE(health != nullptr);
    CHECK(health->value == 10);
    const Velocity* velocity = world.get<Velocity>(spawned);
    REQUIRE(velocity != nullptr);
    CHECK(velocity->y == 2.0F);
}

TEST_CASE("direct recording keeps its place around sections", "[ecs][command]")
{
    World world(resonate::systemAllocator());

    CommandBuffer buffer(world);
    std::vector<std::uint32_t> order;
    const auto step_command = [](void* context, World&, const void* payload)
    {
        std::uint32_t step = 0;
        std::memcpy(&step, payload, sizeof(step));
        static_cast<std::vector<std::uint32_t>*>(context)->push_back(step);
    };
    const auto record_step_at = [&](std::uint32_t step)
    {
        buffer.record(step_command, &order, &step, sizeof(step));
    };

    record_step_at(1);
    {
        auto section = buffer.section(0);
        const std::uint32_t step = 2U;
        section.record(step_command, &order, &step, sizeof(step));
    }
    record_step_at(3);

    world.play(buffer);
    REQUIRE(order == std::vector<std::uint32_t>{1U, 2U, 3U});
}

TEST_CASE("the next-frame channel plays at the next step and survives the step's end",
          "[ecs][command]")
{
    World world(resonate::systemAllocator());
    world.registerComponent<Health>();

    CommandBuffer buffer(world);

    buffer.setChannel(CommandBuffer::Channel::NextFrame);
    const Entity aftermath = buffer.spawn(Health{5});
    buffer.setChannel(CommandBuffer::Channel::SyncPoint);
    const Entity immediate = buffer.spawn(Health{7});

    CHECK(buffer.empty(CommandBuffer::Channel::SyncPoint) == false);
    CHECK(buffer.empty(CommandBuffer::Channel::NextFrame) == false);
    CHECK_FALSE(world.alive(aftermath));
    CHECK_FALSE(world.alive(immediate));

    /* Sync-point playback touches only its channel. */
    world.play(buffer);
    CHECK(world.alive(immediate));
    CHECK_FALSE(world.alive(aftermath));
    CHECK(buffer.empty(CommandBuffer::Channel::SyncPoint));
    CHECK_FALSE(buffer.empty(CommandBuffer::Channel::NextFrame));

    /* Dropping the sync channel's leftovers keeps the aftermath. */
    buffer.clear(CommandBuffer::Channel::SyncPoint);
    CHECK_FALSE(buffer.empty());

    /* Phase 0 of the next step: the aftermath plays. */
    world.playNextFrame(buffer);
    CHECK(world.alive(aftermath));
    CHECK(buffer.empty());
}
