#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/core/job.h>
#include <resonate/ecs/world.h>
#include <resonate/pal/sync.h>
#include <resonate/pal/thread.h>

namespace
{

using resonate::ecs::ChunkView;
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

struct Frozen
{
    std::int32_t flag = 0;
};

/* Named like a component but never registered: a query naming it must fail. */
struct Unregistered
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

/* Sums what an iteration visited, and checks the columns a chunk should and
   should not have. */
struct Visitor
{
    ComponentIndex position = resonate::ecs::kInvalidComponent;
    ComponentIndex missing = resonate::ecs::kInvalidComponent;

    std::uint32_t entities = 0;
    double sum = 0.0;
    std::uint32_t markedTicks = 0;
    bool sawPositionColumn = false;
    bool missingColumnAbsent = true;

    static void run(void* context, ChunkView view)
    {
        auto* self = static_cast<Visitor*>(context);
        self->entities += view.count;

        auto* positions = static_cast<Position*>(view.componentData(self->position));
        self->sawPositionColumn = positions != nullptr;
        if (positions != nullptr)
        {
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                self->sum += positions[row].x;
            }
        }

        if (self->missing != resonate::ecs::kInvalidComponent &&
            (view.componentData(self->missing) != nullptr ||
             view.componentTicks(self->missing) != nullptr))
        {
            self->missingColumnAbsent = false;
        }

        const std::uint32_t* ticks = view.componentTicks(self->position);
        if (ticks != nullptr)
        {
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                if (ticks[row] != 0U)
                {
                    ++self->markedTicks;
                }
            }
        }
    }
};

} // namespace

namespace resonate::ecs
{

RESONATE_COMPONENT(Position, "qtest.Position");
RESONATE_COMPONENT(Velocity, "qtest.Velocity");
RESONATE_COMPONENT(Health, "qtest.Health");
RESONATE_COMPONENT(Frozen, "qtest.Frozen");
RESONATE_COMPONENT(Unregistered, "qtest.Unregistered");

} // namespace resonate::ecs

TEST_CASE("a query picks up matching archetypes as they appear", "[ecs][query]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);
    const ComponentIndex velocity = world.registerComponent<Velocity>();
    REQUIRE(velocity != resonate::ecs::kInvalidComponent);
    const ComponentIndex health = world.registerComponent<Health>();
    REQUIRE(health != resonate::ecs::kInvalidComponent);

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));
    REQUIRE(query.valid());

    /* The query exists before any component does. */
    REQUIRE(query.matchedArchetypeCount() == 0U);

    /* The empty archetype an entity starts in has no Position, so it does not
       match; adding the component produces one that does. */
    const Entity first = world.create();
    REQUIRE(query.matchedArchetypeCount() == 0U);
    REQUIRE(world.add(first, Position{1.0F, 0.0F, 0.0F}) != nullptr);
    REQUIRE(query.matchedArchetypeCount() == 1U);

    /* A second component makes another matching archetype; the one it left
       behind still matches (it exists, empty). */
    REQUIRE(world.add(first, Velocity{0.0F, 0.0F, 0.0F}) != nullptr);
    REQUIRE(query.matchedArchetypeCount() == 2U);

    /* An entity that fits an existing archetype adds no match. */
    const Entity second = world.create();
    REQUIRE(world.add(second, Position{2.0F, 0.0F, 0.0F}) != nullptr);
    REQUIRE(query.matchedArchetypeCount() == 2U);

    /* A genuinely new combination is picked up on the next look. */
    const Entity third = world.create();
    REQUIRE(world.add(third, Health{3}) != nullptr);
    REQUIRE(world.add(third, Position{4.0F, 0.0F, 0.0F}) != nullptr);
    REQUIRE(query.matchedArchetypeCount() == 3U);

    /* One that does not match never does. */
    const Entity fourth = world.create();
    REQUIRE(world.add(fourth, Health{5}) != nullptr);
    REQUIRE(query.matchedArchetypeCount() == 3U);

    Visitor visitor{position};
    query.forEachChunk(&Visitor::run, &visitor);
    REQUIRE(visitor.entities == 3U);
    REQUIRE(visitor.sum == 7.0);
}

TEST_CASE("any and none narrow the match", "[ecs][query]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    const ComponentIndex velocity = world.registerComponent<Velocity>();
    const ComponentIndex health = world.registerComponent<Health>();
    const ComponentIndex frozen = world.registerComponent<Frozen>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    const ComponentIndex all[] = {position};
    const ComponentIndex any[] = {velocity, health};
    const ComponentIndex none[] = {frozen};
    QueryDesc desc;
    desc.all = all;
    desc.any = any;
    desc.none = none;
    Query query = world.createQuery(desc);
    REQUIRE(query.valid());

    /* Matches: Position with Velocity or Health. */
    const Entity moving = world.create();
    REQUIRE(world.add(moving, Position{1.0F, 0.0F, 0.0F}) != nullptr);
    REQUIRE(world.add(moving, Velocity{0.0F, 0.0F, 0.0F}) != nullptr);

    const Entity alive = world.create();
    REQUIRE(world.add(alive, Position{2.0F, 0.0F, 0.0F}) != nullptr);
    REQUIRE(world.add(alive, Health{10}) != nullptr);

    /* Position alone fails `any`. */
    const Entity bare = world.create();
    REQUIRE(world.add(bare, Position{4.0F, 0.0F, 0.0F}) != nullptr);

    /* A matching set that has a `none` component fails. */
    const Entity still = world.create();
    REQUIRE(world.add(still, Position{8.0F, 0.0F, 0.0F}) != nullptr);
    REQUIRE(world.add(still, Health{10}) != nullptr);
    REQUIRE(world.add(still, Frozen{1}) != nullptr);

    Visitor visitor{position};
    query.forEachChunk(&Visitor::run, &visitor);
    REQUIRE(visitor.entities == 2U);
    REQUIRE(visitor.sum == 3.0);
}

TEST_CASE("a query naming an unregistered component is invalid and reports", "[ecs][query]")
{
    Reports reports;
    World world(resonate::systemAllocator());
    world.setReportSink(&reports, &Reports::sink);
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    const ComponentIndex all[] = {
        position, static_cast<ComponentIndex>(resonate::ecs::ComponentTraits<Unregistered>::index)};
    Query query = world.createQuery(describeAll(all, 2));
    REQUIRE_FALSE(query.valid());
    REQUIRE(reports.contains("never registered"));

    /* Iterating an invalid query is a no-op, not a crash. */
    Visitor visitor{position};
    query.forEachChunk(&Visitor::run, &visitor);
    REQUIRE(visitor.entities == 0U);
}

TEST_CASE("a chunk view exposes its columns and their ticks", "[ecs][query]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    /* Registered but not on the entity: its column must be absent. */
    const ComponentIndex velocity = world.registerComponent<Velocity>();
    REQUIRE(velocity != resonate::ecs::kInvalidComponent);

    const Entity entity = world.create();
    REQUIRE(world.add(entity, Position{5.0F, 0.0F, 0.0F}) != nullptr);
    world.markChanged<Position>(entity);

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));

    Visitor visitor{position, velocity};
    query.forEachChunk(&Visitor::run, &visitor);
    REQUIRE(visitor.entities == 1U);
    REQUIRE(visitor.sawPositionColumn);
    REQUIRE(visitor.missingColumnAbsent);
    REQUIRE(visitor.markedTicks == 1U);
    REQUIRE(visitor.sum == 5.0);
}

TEST_CASE("iteration refuses structural changes while it runs", "[ecs][query]")
{
    Reports reports;
    World world(resonate::systemAllocator());
    world.setReportSink(&reports, &Reports::sink);
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    const Entity entity = world.create();
    REQUIRE(world.add(entity, Position{1.0F, 0.0F, 0.0F}) != nullptr);

    struct Attempt
    {
        World* world = nullptr;
        Entity entity{};
        static void run(void* context, ChunkView)
        {
            auto* self = static_cast<Attempt*>(context);
            self->world->destroy(self->entity);
        }
    } attempt{&world, entity};

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));
    query.forEachChunk(&Attempt::run, &attempt);
    REQUIRE(world.alive(entity));

    /* The parallel path holds the same guard. */
    resonate::JobSystem* jobs = resonate::createJobSystem();
    REQUIRE(jobs != nullptr);
    query.parallelEachChunk(*jobs, 0, 0, &Attempt::run, &attempt);
    REQUIRE(world.alive(entity));
    delete jobs;

    REQUIRE(reports.contains("parallel execution is in flight"));
}

TEST_CASE("sequential iteration visits every entity exactly once", "[ecs][query]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    constexpr std::uint32_t COUNT = 3000;
    double expected = 0.0;
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        const Entity entity = world.create();
        REQUIRE(world.add(entity, Position{static_cast<float>(index), 0.0F, 0.0F}) != nullptr);
        expected += static_cast<double>(index);
    }

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));

    Visitor visitor{position};
    query.forEachChunk(&Visitor::run, &visitor);
    REQUIRE(visitor.entities == COUNT);
    REQUIRE(visitor.sum == expected);
    REQUIRE(query.matchedChunkCount() > 1U);
}

TEST_CASE("parallel iteration visits every entity once across the pool", "[ecs][query]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    constexpr std::uint32_t COUNT = 20000;
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        const Entity entity = world.create();
        REQUIRE(world.add(entity, Position{1.0F, 0.0F, 0.0F}) != nullptr);
    }

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));
    REQUIRE(query.matchedChunkCount() > 1U);

    struct Doubler
    {
        ComponentIndex position = resonate::ecs::kInvalidComponent;
        std::atomic<std::uint32_t> visited{0};

        static void run(void* context, ChunkView view)
        {
            auto* self = static_cast<Doubler*>(context);
            auto* positions = static_cast<Position*>(view.componentData(self->position));
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                positions[row].x *= 2.0F;
            }
            self->visited.fetch_add(view.count, std::memory_order_relaxed);
        }
    } doubler{position};

    resonate::JobSystem* jobs = resonate::createJobSystem();
    REQUIRE(jobs != nullptr);
    query.parallelEachChunk(*jobs, 0, 0, &Doubler::run, &doubler);
    REQUIRE(doubler.visited.load() == COUNT);

    /* Every entity was doubled exactly once. */
    Visitor visitor{position};
    query.forEachChunk(&Visitor::run, &visitor);
    REQUIRE(visitor.entities == COUNT);
    REQUIRE(visitor.sum == 2.0 * static_cast<double>(COUNT));

    delete jobs;
}

TEST_CASE("parallel iteration still covers everything with no workers awake", "[ecs][query]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    constexpr std::uint32_t COUNT = 3000;
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        const Entity entity = world.create();
        REQUIRE(world.add(entity, Position{1.0F, 0.0F, 0.0F}) != nullptr);
    }

    resonate::JobSystem* jobs = resonate::createJobSystem();
    REQUIRE(jobs != nullptr);
    jobs->requestWorkerCount(0);
    int settled = 0;
    for (int spin = 0; spin < 10000000 && settled < 1000; ++spin)
    {
        settled = jobs->workerCount() == 0 ? settled + 1 : 0;
    }
    REQUIRE(settled == 1000);

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));

    struct Counter
    {
        ComponentIndex position = resonate::ecs::kInvalidComponent;
        std::atomic<std::uint32_t> visited{0};
        static void run(void* context, ChunkView view)
        {
            static_cast<Counter*>(context)->visited.fetch_add(view.count,
                                                              std::memory_order_relaxed);
        }
    } counter{position};

    query.parallelEachChunk(*jobs, 0, 0, &Counter::run, &counter);
    REQUIRE(counter.visited.load() == COUNT);

    delete jobs;
}

TEST_CASE("two chunks can be visited at the same time", "[ecs][query]")
{
    if (resonate_pal_sync_hardware_concurrency() < 2)
    {
        SKIP("needs a second core");
    }

    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    /* Enough entities that the query has several chunks to hand out. */
    constexpr std::uint32_t COUNT = 3000;
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        const Entity entity = world.create();
        REQUIRE(world.add(entity, Position{1.0F, 0.0F, 0.0F}) != nullptr);
    }

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));
    REQUIRE(query.matchedChunkCount() > 1U);

    struct Rendezvous
    {
        std::atomic<std::uint32_t> entered{0};
        std::atomic<bool> sawEachOther{false};

        static void run(void* context, ChunkView)
        {
            auto* self = static_cast<Rendezvous*>(context);

            /* Arriving while somebody is already inside is the observation;
               the first one waits a bounded while to give the others a chance,
               so a schedule that never overlaps fails rather than hangs. */
            if (self->entered.fetch_add(1U) + 1U >= 2U)
            {
                self->sawEachOther.store(true);
            }
            else
            {
                for (int spin = 0; spin < 500000 && self->entered.load() < 2U; ++spin)
                {
                    resonate_pal_thread_yield();
                }
            }
            self->entered.fetch_sub(1U);
        }
    } rendezvous;

    resonate::JobSystem* jobs = resonate::createJobSystem();
    REQUIRE(jobs != nullptr);
    query.parallelEachChunk(*jobs, 0, 0, &Rendezvous::run, &rendezvous);
    REQUIRE(rendezvous.sawEachOther.load());

    delete jobs;
}

TEST_CASE("iterating one hundred thousand entities", "[ecs][query][benchmark]")
{
    constexpr std::uint32_t COUNT = 100000;

    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);

    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        const Entity entity = world.create();
        REQUIRE(world.add(entity, Position{1.0F, 0.0F, 0.0F}) != nullptr);
    }

    const ComponentIndex all[] = {position};
    Query query = world.createQuery(describeAll(all, 1));

    Visitor visitor{position};
    auto start = std::chrono::steady_clock::now();
    query.forEachChunk(&Visitor::run, &visitor);
    auto elapsed = std::chrono::steady_clock::now() - start;
    const double sequential_seconds = std::chrono::duration<double>(elapsed).count();
    REQUIRE(visitor.entities == COUNT);

    resonate::JobSystem* jobs = resonate::createJobSystem();
    REQUIRE(jobs != nullptr);

    struct Counter
    {
        ComponentIndex position = resonate::ecs::kInvalidComponent;
        std::atomic<std::uint32_t> visited{0};
        static void run(void* context, ChunkView view)
        {
            static_cast<Counter*>(context)->visited.fetch_add(view.count,
                                                              std::memory_order_relaxed);
        }
    } counter{position};

    start = std::chrono::steady_clock::now();
    query.parallelEachChunk(*jobs, 0, 0, &Counter::run, &counter);
    elapsed = std::chrono::steady_clock::now() - start;
    const double parallel_seconds = std::chrono::duration<double>(elapsed).count();
    REQUIRE(counter.visited.load() == COUNT);

    std::printf("[ecs] iterate %u entities: %.1f M/s sequential, %.1f M/s parallel (%.1f ms)\n",
                COUNT, static_cast<double>(COUNT) / sequential_seconds / 1e6,
                static_cast<double>(COUNT) / parallel_seconds / 1e6, parallel_seconds * 1e3);

    /* Floors, not the design's bandwidth reference line: they catch an
       iteration that stopped iterating, on a loaded sanitizer build too. */
    REQUIRE(static_cast<double>(COUNT) / sequential_seconds > 10.0e6);
    REQUIRE(static_cast<double>(COUNT) / parallel_seconds > 10.0e6);

    delete jobs;
}
