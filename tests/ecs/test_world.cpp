#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/world.h>

namespace
{

using resonate::ecs::ComponentIndex;
using resonate::ecs::ComponentTraits;
using resonate::ecs::Entity;
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

/* A type whose declared layout cannot be stored as an array stride. */
struct OddLayout
{
    std::uint32_t value = 0;
};

/* Two types claiming one name: the second registration must be refused. Their
   layouts differ, which is what makes the collision detectable — the name alone
   is a type's identity. */
struct TwinA
{
    std::uint32_t value = 0;
};

struct TwinB
{
    std::uint64_t value = 0;
};

/* Larger than a chunk: no archetype carrying it can hold an entity. */
struct Huge
{
    unsigned char bytes[32U * 1024U];
};

/* What the world reports is only observable through its sink. */
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

} // namespace

namespace resonate::ecs
{

RESONATE_COMPONENT(Position, "test.Position");
RESONATE_COMPONENT(Velocity, "test.Velocity");
RESONATE_COMPONENT(Health, "test.Health");
RESONATE_COMPONENT(TwinA, "test.Twin");
RESONATE_COMPONENT(TwinB, "test.Twin");
RESONATE_COMPONENT(Huge, "test.Huge");

/* The declaration is the exception: this one overrides its layout. */
template <> struct ComponentTraits<OddLayout> : detail::ComponentDefaults<OddLayout>
{
    static constexpr const char* name = "test.OddLayout";
    static constexpr std::uint32_t size = 6;
    static constexpr std::uint32_t alignment = 4;
};

} // namespace resonate::ecs

TEST_CASE("a destroyed handle is dead and a recycled index gets a new generation", "[ecs][world]")
{
    Reports reports;
    World world(resonate::systemAllocator());
    world.setReportSink(&reports, &Reports::sink);

    const Entity first = world.create();
    REQUIRE(first.valid());
    REQUIRE(first.generation != 0U);
    REQUIRE(world.alive(first));
    REQUIRE(world.entityCount() == 1U);

    world.destroy(first);
    REQUIRE_FALSE(world.alive(first));
    REQUIRE(world.entityCount() == 0U);

    /* The index comes back, the identity does not. */
    const Entity second = world.create();
    REQUIRE(second.index == first.index);
    REQUIRE(second.generation != first.generation);
    REQUIRE(world.alive(second));

    /* Every accessor refuses the stale handle, and says so. */
    reports.lines.clear();
    REQUIRE(world.get<Health>(first) == nullptr);
    REQUIRE(world.add<Health>(first, Health{1}) == nullptr);
    REQUIRE_FALSE(world.remove<Health>(first));
    REQUIRE_FALSE(world.has<Health>(first));
    world.destroy(first);
    world.markChanged<Health>(first);
    REQUIRE(world.alive(second));
    REQUIRE(reports.contains("is not alive"));
}

TEST_CASE("components follow the entity across archetype moves", "[ecs][world]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Position>() != resonate::ecs::kInvalidComponent);
    REQUIRE(world.registerComponent<Velocity>() != resonate::ecs::kInvalidComponent);

    const Entity entity = world.create();
    REQUIRE(world.archetypeCount() == 1U); /* the empty archetype */

    const Position position{1.0F, 2.0F, 3.0F};
    const Velocity velocity{4.0F, 5.0F, 6.0F};
    REQUIRE(world.add(entity, position) != nullptr);
    REQUIRE(world.archetypeCount() == 2U);
    REQUIRE(world.get<Position>(entity) != nullptr);
    REQUIRE(world.get<Position>(entity)->x == 1.0F);
    REQUIRE_FALSE(world.has<Velocity>(entity));

    /* Adding a second component moves the entity again; what it had is copied. */
    REQUIRE(world.add(entity, velocity) != nullptr);
    REQUIRE(world.archetypeCount() == 3U);
    REQUIRE(world.get<Velocity>(entity) != nullptr);
    REQUIRE(world.get<Velocity>(entity)->z == 6.0F);
    REQUIRE(world.get<Position>(entity) != nullptr);
    REQUIRE(world.get<Position>(entity)->y == 2.0F);

    /* Removing one keeps the other. */
    REQUIRE(world.remove<Position>(entity));
    REQUIRE_FALSE(world.has<Position>(entity));
    REQUIRE(world.get<Position>(entity) == nullptr);
    REQUIRE(world.get<Velocity>(entity) != nullptr);
    REQUIRE(world.get<Velocity>(entity)->y == 5.0F);
    REQUIRE(world.entityCount() == 1U);

    /* Removing what is not there is a no-op, not a failure. */
    REQUIRE_FALSE(world.remove<Position>(entity));

    const Position replacement{-1.0F, -2.0F, -3.0F};
    REQUIRE(world.add(entity, replacement) != nullptr);
    REQUIRE(world.get<Position>(entity)->z == -3.0F);
    REQUIRE(world.get<Velocity>(entity)->x == 4.0F);

    /* Adding what is already there returns the stored component unchanged. */
    const Position* stored = world.add(entity, Position{9.0F, 9.0F, 9.0F});
    REQUIRE(stored != nullptr);
    REQUIRE(stored->x == -1.0F);
}

TEST_CASE("thousands of entities keep their own data across chunks", "[ecs][world]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Position>() != resonate::ecs::kInvalidComponent);

    constexpr std::uint32_t COUNT = 3000;
    std::vector<Entity> entities(COUNT);
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        entities[index] = world.create();
        REQUIRE(entities[index].valid());
        const Position position{static_cast<float>(index), 1.0F, 2.0F};
        REQUIRE(world.add(entities[index], position) != nullptr);
    }
    REQUIRE(world.entityCount() == COUNT);
    REQUIRE(world.liveChunkCount() > 1U);

    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        const Position* position = world.get<Position>(entities[index]);
        REQUIRE(position != nullptr);
        REQUIRE(position->x == static_cast<float>(index));
    }

    /* Destroying every other entity compacts its chunk in place; the survivors
       keep both their handle and their data. */
    for (std::uint32_t index = 0; index < COUNT; index += 2)
    {
        world.destroy(entities[index]);
    }
    REQUIRE(world.entityCount() == COUNT / 2U);
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        if ((index % 2U) == 0U)
        {
            REQUIRE_FALSE(world.alive(entities[index]));
            continue;
        }
        REQUIRE(world.alive(entities[index]));
        const Position* position = world.get<Position>(entities[index]);
        REQUIRE(position != nullptr);
        REQUIRE(position->x == static_cast<float>(index));
    }
}

TEST_CASE("a row that moves with a swap-removal keeps its own data", "[ecs][world]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Position>() != resonate::ecs::kInvalidComponent);

    /* Few enough to share one chunk, so the last row is the one that moves. */
    constexpr std::uint32_t COUNT = 100;
    std::vector<Entity> entities(COUNT);
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        entities[index] = world.create();
        REQUIRE(world.add(entities[index], Position{static_cast<float>(index), 0.0F, 0.0F}) !=
                nullptr);
    }

    /* Destroying the first row moves the last one into it; a new entity takes
       the vacated row, which is where a slot that was not repointed would now
       read from. */
    world.destroy(entities[0]);
    const Entity newcomer = world.create();
    REQUIRE(newcomer.valid());
    REQUIRE(world.add(newcomer, Position{-12345.0F, 0.0F, 0.0F}) != nullptr);

    REQUIRE_FALSE(world.alive(entities[0]));
    REQUIRE(world.alive(entities[COUNT - 1U]));
    const Position* moved = world.get<Position>(entities[COUNT - 1U]);
    REQUIRE(moved != nullptr);
    REQUIRE(moved->x == static_cast<float>(COUNT - 1U));

    const Position* fresh = world.get<Position>(newcomer);
    REQUIRE(fresh != nullptr);
    REQUIRE(fresh->x == -12345.0F);

    /* The moved entity's component is the one the newcomer overwrote, not a
       copy of it: writing through it must not touch the newcomer. */
    world.get<Position>(entities[COUNT - 1U])->x = 7.0F;
    REQUIRE(world.get<Position>(newcomer)->x == -12345.0F);
}

TEST_CASE("empty chunks return to the pool and are reused", "[ecs][world]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Position>() != resonate::ecs::kInvalidComponent);

    constexpr std::uint32_t COUNT = 3000;
    std::vector<Entity> entities(COUNT);
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        entities[index] = world.create();
        REQUIRE(world.add(entities[index], Position{}) != nullptr);
    }
    const std::uint32_t chunks_while_full = world.liveChunkCount();
    REQUIRE(chunks_while_full > 1U);
    REQUIRE(world.pooledChunkCount() == 0U);

    for (const Entity entity : entities)
    {
        world.destroy(entity);
    }
    REQUIRE(world.entityCount() == 0U);
    REQUIRE(world.pooledChunkCount() > 0U);

    /* The next batch draws on the pool instead of the allocator. */
    const std::uint32_t pooled_after_drain = world.pooledChunkCount();
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        entities[index] = world.create();
        REQUIRE(world.add(entities[index], Position{}) != nullptr);
    }
    REQUIRE(world.pooledChunkCount() < pooled_after_drain);
    REQUIRE(world.liveChunkCount() <= chunks_while_full);
}

TEST_CASE("change ticks stamp the entity and the type", "[ecs][world]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Position>() != resonate::ecs::kInvalidComponent);
    REQUIRE(world.registerComponent<Velocity>() != resonate::ecs::kInvalidComponent);

    const Entity entity = world.create();
    REQUIRE(world.add(entity, Position{1.0F, 0.0F, 0.0F}) != nullptr);
    const Entity untouched = world.create();
    REQUIRE(world.add(untouched, Position{2.0F, 0.0F, 0.0F}) != nullptr);

    /* Adding a component with a value is a change. */
    REQUIRE(world.changedSince<Position>(entity, 0U));
    const resonate::ecs::ChangeTick after_add = world.componentTick<Position>();

    /* A write the owner marks is a change; another entity is not swept up. */
    world.markChanged<Position>(entity);
    REQUIRE(world.componentTick<Position>() != after_add);
    REQUIRE(world.changedSince<Position>(entity, after_add));
    REQUIRE_FALSE(world.changedSince<Position>(untouched, after_add));

    /* Other types have their own clock. */
    REQUIRE(world.componentTick<Velocity>() == 0U);
    REQUIRE_FALSE(world.changedSince<Velocity>(entity, 0U));

    /* Marking a component the entity does not have is refused. */
    Reports reports;
    world.setReportSink(&reports, &Reports::sink);
    world.markChanged<Velocity>(entity);
    REQUIRE(reports.contains("has no component"));
}

TEST_CASE("tick order is the signed distance, so it survives the wrap", "[ecs][world]")
{
    using resonate::ecs::tickAfter;
    REQUIRE(tickAfter(1U, 0U));
    REQUIRE_FALSE(tickAfter(0U, 1U));
    REQUIRE_FALSE(tickAfter(7U, 7U));

    /* 1 is one step after the largest tick there is. */
    REQUIRE(tickAfter(1U, 0xFFFFFFFFU));
    REQUIRE_FALSE(tickAfter(0xFFFFFFFFU, 1U));

    /* Exactly half the range apart is the ambiguity point of the convention:
       neither direction reads as "after", while the step before it does. */
    REQUIRE(tickAfter(0x7FFFFFFFU, 0U));
    REQUIRE_FALSE(tickAfter(0U, 0x7FFFFFFFU));
    REQUIRE_FALSE(tickAfter(0U, 0x80000000U));
    REQUIRE_FALSE(tickAfter(0x80000000U, 0U));
}

TEST_CASE("component registration is idempotent and refuses name collisions", "[ecs][world]")
{
    Reports reports;
    World world(resonate::systemAllocator());
    world.setReportSink(&reports, &Reports::sink);

    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);
    REQUIRE(world.registerComponent<Position>() == position);
    REQUIRE(world.componentTypeCount() == 1U);
    REQUIRE(world.findComponent("test.Position") == position);
    REQUIRE(world.findComponent("test.Missing") == resonate::ecs::kInvalidComponent);

    /* A second type claiming the name is refused, not aliased. */
    REQUIRE(world.registerComponent<TwinA>() != resonate::ecs::kInvalidComponent);
    REQUIRE(world.registerComponent<TwinB>() == resonate::ecs::kInvalidComponent);
    REQUIRE(reports.contains("already registered"));
    REQUIRE(world.componentTypeCount() == 2U);

    /* A layout that cannot be an array stride is refused. */
    REQUIRE(world.registerComponent<OddLayout>() == resonate::ecs::kInvalidComponent);
    REQUIRE(reports.contains("unusable layout"));
}

TEST_CASE("a component larger than a chunk is refused without moving the entity", "[ecs][world]")
{
    Reports reports;
    World world(resonate::systemAllocator());
    world.setReportSink(&reports, &Reports::sink);
    REQUIRE(world.registerComponent<Huge>() != resonate::ecs::kInvalidComponent);
    REQUIRE(world.registerComponent<Position>() != resonate::ecs::kInvalidComponent);

    const Entity entity = world.create();
    REQUIRE(world.add(entity, Position{1.0F, 2.0F, 3.0F}) != nullptr);

    REQUIRE(world.add(entity, Huge{}) == nullptr);
    REQUIRE(reports.contains("no room for one entity"));
    REQUIRE(world.alive(entity));
    REQUIRE(world.get<Position>(entity) != nullptr);
    REQUIRE(world.get<Position>(entity)->y == 2.0F);

    /* One the entity keeps: removal still works. */
    REQUIRE(world.remove<Position>(entity));
}

TEST_CASE("structural change is refused while parallel execution is in flight", "[ecs][world]")
{
    Reports reports;
    World world(resonate::systemAllocator());
    world.setReportSink(&reports, &Reports::sink);

    REQUIRE(world.registerComponent<Position>() != resonate::ecs::kInvalidComponent);
    const Entity entity = world.create();
    REQUIRE(world.add(entity, Position{1.0F, 0.0F, 0.0F}) != nullptr);

    {
        const World::ParallelScope parallel(world);
        REQUIRE(world.inParallelExecution());

        REQUIRE_FALSE(world.create().valid());
        world.destroy(entity);
        REQUIRE(world.alive(entity));
        REQUIRE(world.add(entity, Velocity{}) == nullptr);
        REQUIRE_FALSE(world.remove<Position>(entity));
        REQUIRE(reports.contains("parallel execution is in flight"));
        REQUIRE(world.entityCount() == 1U);
        REQUIRE(world.get<Position>(entity) != nullptr);

        /* Reads and change marks are not structural and stay legal. */
        world.markChanged<Position>(entity);
        REQUIRE(world.changedSince<Position>(entity, 0U));
    }

    REQUIRE_FALSE(world.inParallelExecution());
    REQUIRE(world.create().valid());
    REQUIRE(world.remove<Position>(entity));
}

TEST_CASE("one hundred thousand entities: create, component churn and destroy",
          "[ecs][world][benchmark]")
{
    constexpr std::uint32_t COUNT = 100000;

    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Position>() != resonate::ecs::kInvalidComponent);
    REQUIRE(world.registerComponent<Velocity>() != resonate::ecs::kInvalidComponent);

    std::vector<Entity> entities(COUNT);
    const auto start = std::chrono::steady_clock::now();

    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        entities[index] = world.create();
        REQUIRE(entities[index].valid());
    }
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        REQUIRE(world.add(entities[index], Position{static_cast<float>(index), 0.0F, 0.0F}) !=
                nullptr);
    }
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        REQUIRE(world.add(entities[index], Velocity{0.0F, static_cast<float>(index), 0.0F}) !=
                nullptr);
    }
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        Position* position = world.get<Position>(entities[index]);
        REQUIRE(position != nullptr);
        position->y = position->x + 1.0F;
    }
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        world.destroy(entities[index]);
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double operations = static_cast<double>(COUNT) * 5.0;
    const double rate = operations / seconds;
    std::printf("[ecs] %u entities, create+2 adds+get+destroy: %.1f Mops/s (%.1f ms)\n", COUNT,
                rate / 1e6, seconds * 1e3);

    REQUIRE(world.entityCount() == 0U);
    /* A wide floor: it catches a storage that stopped storing, while surviving
       a sanitizer build on a loaded machine. */
    REQUIRE(rate > 1.0e6);
}

TEST_CASE("named RNG streams derive from the seed and replay", "[ecs][world]")
{
    using resonate::Rng;

    World world(resonate::systemAllocator());

    CHECK(world.rngStream(nullptr) == nullptr);
    CHECK(world.rngStream("") == nullptr);

    /* A stream keeps its state between frames: two lookups are one stream. */
    world.setSeed(1234);
    Rng* combat = world.rngStream("test.combat");
    REQUIRE(combat != nullptr);
    Rng* combat_again = world.rngStream("test.combat");
    REQUIRE(combat_again == combat);
    Rng* ambient = world.rngStream("test.ambient");
    REQUIRE(ambient != nullptr);
    REQUIRE(ambient != combat);

    /* The same seed derives the same streams; a different seed does not. */
    World other(resonate::systemAllocator());
    other.setSeed(1234);
    const std::uint64_t expected = combat->nextU64();
    CHECK(other.rngStream("test.combat")->nextU64() == expected);

    World divergent(resonate::systemAllocator());
    divergent.setSeed(4321);
    CHECK(divergent.rngStream("test.combat")->nextU64() != expected);

    /* Re-seeding resets the streams already handed out: a replay's reset. */
    const std::uint64_t first = combat->nextU64();
    world.setSeed(1234);
    CHECK(combat->nextU64() == expected);
    CHECK(first != expected);
}
