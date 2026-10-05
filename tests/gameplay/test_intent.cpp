#include <catch2/catch_all.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <resonate/core/job.h>
#include <resonate/gameplay/intent.h>

namespace ecs = resonate::ecs;

namespace
{

constexpr resonate::JobGroup GROUP_SHARED = 1U << 0;

using resonate::ecs::Entity;

struct Health
{
    std::int32_t value = 0;
};

} // namespace

namespace resonate::ecs
{

RESONATE_COMPONENT(Health, "itest.Health");

} // namespace resonate::ecs

namespace
{
using resonate::ecs::World;
using resonate::gameplay::Intent;
using resonate::gameplay::IntentBus;

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

struct PayloadLog
{
    /* What the handlers saw, in dispatch order. */
    std::vector<std::string> seen;
};

void logHandler(void* user, World&, ecs::CommandBuffer::Section&, const Intent& intent)
{
    auto* log = static_cast<PayloadLog*>(user);
    int value = 0;
    if (intent.payload != nullptr)
    {
        std::memcpy(&value, intent.payload, sizeof(value));
    }
    char line[64] = {};
    std::snprintf(line, sizeof(line), "%u:%d->%u", intent.type, value, intent.target.index);
    log->seen.emplace_back(line);
}

} // namespace

TEST_CASE("intents are adjudicated FIFO by their handler", "[gameplay][intent]")
{
    World world(resonate::systemAllocator());
    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    const Entity target_a = world.create();
    const Entity target_b = world.create();
    REQUIRE(target_a.valid());
    REQUIRE(target_b.valid());

    IntentBus bus(world, resonate::systemAllocator());
    const resonate::gameplay::IntentTypeId damage =
        bus.registerType("test.damage", sizeof(int), resonate::gameplay::UnhandledPolicy::Ignore);
    REQUIRE(damage != resonate::gameplay::kInvalidIntent);

    PayloadLog log;
    resonate::gameplay::IntentBus::HandlerDesc desc;
    desc.type = damage;
    desc.name = "test.damage.handler";
    desc.run = &logHandler;
    desc.user = &log;
    REQUIRE(bus.addHandler(desc));

    const int first = 1;
    const int second = 2;
    const int third = 3;
    REQUIRE(bus.submit(damage, Entity{}, target_a, &first));
    REQUIRE(bus.submit(damage, target_a, target_b, &second));
    REQUIRE(bus.submit(damage, Entity{}, target_a, &third));

    bus.adjudicate(nullptr);

    REQUIRE(log.seen.size() == 3);
    CHECK(log.seen[0] == "0:1->" + std::to_string(target_a.index));
    CHECK(log.seen[1] == "0:2->" + std::to_string(target_b.index));
    CHECK(log.seen[2] == "0:3->" + std::to_string(target_a.index));
    CHECK(bus.lastStats().waves == 1U);
    CHECK(bus.lastStats().dispatched == 3U);
    CHECK(bus.pendingCount() == 0U);
    CHECK(bus.commands().empty());

    /* A stale endpoint is refused at submit, with a report. */
    world.destroy(target_b);
    CHECK_FALSE(bus.submit(damage, Entity{}, target_b, &first));
    CHECK(reports.contains("which is not alive"));
}

TEST_CASE("handlerless intents follow the type's policy", "[gameplay][intent]")
{
    World world(resonate::systemAllocator());
    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    const Entity target = world.create();
    IntentBus bus(world, resonate::systemAllocator());

    const resonate::gameplay::IntentTypeId quiet = bus.registerType("test.quiet", sizeof(int));
    const resonate::gameplay::IntentTypeId loud = bus.registerType(
        "test.loud", sizeof(int), resonate::gameplay::UnhandledPolicy::Reject);
    REQUIRE(quiet != resonate::gameplay::kInvalidIntent);
    REQUIRE(loud != resonate::gameplay::kInvalidIntent);

    const int value = 7;
    REQUIRE(bus.submit(quiet, Entity{}, target, &value));
    REQUIRE(bus.submit(loud, Entity{}, target, &value));

    bus.adjudicate(nullptr);

    CHECK(bus.lastStats().dispatched == 0U);
    CHECK(bus.lastStats().dropped == 2U);
    CHECK(reports.contains("test.loud"));
    CHECK_FALSE(reports.contains("test.quiet"));
}

TEST_CASE("a handler's submissions land in the next wave", "[gameplay][intent]")
{
    World world(resonate::systemAllocator());
    const Entity target = world.create();

    IntentBus bus(world, resonate::systemAllocator());
    const auto type =
        bus.registerType("test.cascade", sizeof(int), resonate::gameplay::UnhandledPolicy::Ignore);

    /* Each intent carries the remaining depth; the handler passes one less
       down until it hits one. */
    resonate::gameplay::IntentBus::HandlerDesc desc;
    desc.type = type;
    desc.name = "test.cascade.handler";
    desc.run =
        [](void* user, World&, ecs::CommandBuffer::Section&, const Intent& intent)
    {
        auto* bus = static_cast<IntentBus*>(user);
        int remaining = 0;
        std::memcpy(&remaining, intent.payload, sizeof(remaining));
        if (remaining > 1)
        {
            const int next = remaining - 1;
            bus->submit(intent.type, intent.source, intent.target, &next);
        }
    };
    desc.user = &bus;
    REQUIRE(bus.addHandler(desc));

    const int depth = 4;
    REQUIRE(bus.submit(type, Entity{}, target, &depth));

    bus.setDepthCap(16);
    bus.adjudicate(nullptr);

    /* One wave per link, and the last wave's intent stops the cascade. */
    CHECK(bus.lastStats().waves == 4U);
    CHECK(bus.lastStats().dispatched == 4U);
    CHECK(bus.pendingCount() == 0U);
}

TEST_CASE("the depth cap spills to the aftermath and Phase 0 settles it", "[gameplay][intent]")
{
    World world(resonate::systemAllocator());
    const Entity target = world.create();

    IntentBus bus(world, resonate::systemAllocator());
    const auto type =
        bus.registerType("test.cascade", sizeof(int), resonate::gameplay::UnhandledPolicy::Ignore);
    resonate::gameplay::IntentBus::HandlerDesc desc;
    desc.type = type;
    desc.name = "test.cascade.handler";
    desc.run = [](void* user, World&, ecs::CommandBuffer::Section&, const Intent& intent)
    {
        static_cast<IntentBus*>(user)->submit(intent.type, intent.source, intent.target,
                                              intent.payload);
    };
    desc.user = &bus;
    REQUIRE(bus.addHandler(desc));

    const int depth = 5;
    REQUIRE(bus.submit(type, Entity{}, target, &depth));

    /* Two waves, then the fuse: what the second wave produced waits. */
    bus.setDepthCap(2);
    bus.adjudicate(nullptr);
    CHECK(bus.lastStats().waves == 2U);
    CHECK(bus.lastStats().spilled == 1U);
    CHECK(bus.pendingCount() == 0U);

    /* A sync point does not touch the aftermath; Phase 0 settles it. */
    bus.adjudicate(nullptr);
    CHECK(bus.lastStats().waves == 0U);
    CHECK(bus.lastStats().settled == 0U);

    bus.settleAftermath(nullptr);
    CHECK(bus.lastStats().settled == 1U);
    /* The settled intent resubmits itself, so it runs the two waves the cap
       allows and spills again: the fuse holds at every adjudication. */
    CHECK(bus.lastStats().waves == 2U);
    CHECK(bus.lastStats().spilled == 1U);
    CHECK(bus.pendingCount() == 0U);
}

TEST_CASE("a target that died between submit and dispatch is dropped", "[gameplay][intent]")
{
    World world(resonate::systemAllocator());
    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    const Entity target = world.create();
    IntentBus bus(world, resonate::systemAllocator());
    const auto type = bus.registerType("test.damage", sizeof(int));

    PayloadLog log;
    resonate::gameplay::IntentBus::HandlerDesc desc;
    desc.type = type;
    desc.name = "test.damage.handler";
    desc.run = &logHandler;
    desc.user = &log;
    REQUIRE(bus.addHandler(desc));

    const int value = 1;
    REQUIRE(bus.submit(type, Entity{}, target, &value));
    world.destroy(target);

    bus.adjudicate(nullptr);
    CHECK(log.seen.empty());
    CHECK(bus.lastStats().dropped == 1U);
    CHECK(reports.contains("which is not alive"));
}

TEST_CASE("a handler's structural changes record into the bus's buffer", "[gameplay][intent]")
{
    World world(resonate::systemAllocator());
    world.registerComponent<Health>();

    const Entity target = world.create();
    IntentBus bus(world, resonate::systemAllocator());
    const auto type = bus.registerType("test.spawn", sizeof(int));

    resonate::gameplay::IntentBus::HandlerDesc desc;
    desc.type = type;
    desc.name = "test.spawn.handler";
    desc.run = [](void*, World&, ecs::CommandBuffer::Section& commands, const Intent&)
    {
        commands.spawn(Health{42});
    };
    REQUIRE(bus.addHandler(desc));

    const int value = 0;
    REQUIRE(bus.submit(type, Entity{}, target, &value));

    /* The recording happened at the wave; the buffer plays at a sync point. */
    CHECK(world.entityCount() == 1U);
    bus.adjudicate(nullptr);
    CHECK_FALSE(bus.commands().empty());

    world.play(bus.commands());
    CHECK(world.entityCount() == 2U);
    CHECK(bus.commands().empty());
}

TEST_CASE("handlers sharing a group run one after another", "[gameplay][intent]")
{
    World world(resonate::systemAllocator());
    const Entity target = world.create();

    IntentBus bus(world, resonate::systemAllocator());
    const auto first_type = bus.registerType("test.first", sizeof(int));
    const auto second_type = bus.registerType("test.second", sizeof(int));

    struct Rank
    {
        std::vector<std::uint32_t> order;
    } rank;

    const auto ranking_handler = [](void* user, World&, ecs::CommandBuffer::Section&,
                                    const Intent& intent)
    {
        auto* rank = static_cast<Rank*>(user);
        rank->order.push_back(intent.type);
    };

    resonate::gameplay::IntentBus::HandlerDesc first;
    first.type = first_type;
    first.name = "test.first.handler";
    first.run = ranking_handler;
    first.user = &rank;
    first.reads = 0;
    first.writes = GROUP_SHARED;
    REQUIRE(bus.addHandler(first));

    resonate::gameplay::IntentBus::HandlerDesc second;
    second.type = second_type;
    second.name = "test.second.handler";
    second.run = ranking_handler;
    second.user = &rank;
    second.reads = 0;
    second.writes = GROUP_SHARED;
    REQUIRE(bus.addHandler(second));

    const int value = 0;
    REQUIRE(bus.submit(first_type, Entity{}, target, &value));
    REQUIRE(bus.submit(second_type, Entity{}, target, &value));

    resonate::JobSystem* jobs = resonate::createJobSystem();
    bus.adjudicate(jobs);
    delete jobs;

    /* Same group, both writing: the pool serializes them in submission order,
       which is the wave's registration order. */
    REQUIRE(rank.order.size() == 2U);
    CHECK(rank.order[0] == first_type);
    CHECK(rank.order[1] == second_type);
}

TEST_CASE("the same submissions replay the same adjudication", "[gameplay][intent]")
{
    World world(resonate::systemAllocator());
    const Entity target = world.create();

    const auto run_once = [&world, target]()
    {
        IntentBus bus(world, resonate::systemAllocator());
        const auto type = bus.registerType("test.replay", sizeof(int));
        resonate::gameplay::IntentBus::HandlerDesc desc;
        desc.type = type;
        desc.name = "test.replay.handler";
        desc.run = &logHandler;
        PayloadLog log;
        desc.user = &log;
        REQUIRE(bus.addHandler(desc));

        for (int index = 0; index < 5; ++index)
        {
            REQUIRE(bus.submit(type, Entity{}, target, &index));
        }
        resonate::JobSystem* jobs = resonate::createJobSystem();
        bus.adjudicate(jobs);
        delete jobs;
        return log.seen;
    };

    const std::vector<std::string> first = run_once();
    const std::vector<std::string> second = run_once();
    REQUIRE(first.size() == 5U);
    CHECK(first == second);
}
