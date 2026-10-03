#include <catch2/catch_all.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>

namespace
{

using resonate::ecs::CommandBuffer;
using resonate::ecs::ComponentIndex;
using resonate::ecs::Entity;
using resonate::ecs::World;

struct Health
{
    std::int32_t value = 0;
};

struct Mana
{
    std::int32_t value = 0;
};

struct Seen
{
    Entity entity{};
    ComponentIndex component = resonate::ecs::kInvalidComponent;
};

struct Watcher
{
    std::vector<Seen> seen;
    std::vector<std::string> reports;

    static void observe(void* user_data, World&, Entity entity, ComponentIndex component)
    {
        static_cast<Watcher*>(user_data)->seen.push_back(Seen{entity, component});
    }

    static void sink(void* user_data, World::Report, const char* message)
    {
        static_cast<Watcher*>(user_data)->reports.emplace_back(message != nullptr ? message : "");
    }

    [[nodiscard]] bool reported(const char* fragment) const
    {
        for (const std::string& line : reports)
        {
            if (line.find(fragment) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
};

/* An observer that tries to change structure while it runs: the world is in the
   middle of announcing a change, and the attempt must be refused. */
struct Reentrant
{
    Entity target{};
    ComponentIndex component = resonate::ecs::kInvalidComponent;
    bool survived = false;

    static void observe(void* user_data, World& world, Entity, ComponentIndex)
    {
        auto* self = static_cast<Reentrant*>(user_data);
        self->survived = world.add(self->target, self->component, nullptr) != nullptr;
    }
};

} // namespace

namespace resonate::ecs
{

RESONATE_COMPONENT(Health, "otest.Health");
RESONATE_COMPONENT(Mana, "otest.Mana");

} // namespace resonate::ecs

TEST_CASE("a component arrival and a write notify the type's observers", "[ecs][observer]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex health = world.registerComponent<Health>();
    const ComponentIndex mana = world.registerComponent<Mana>();
    REQUIRE(health != resonate::ecs::kInvalidComponent);
    REQUIRE(mana != resonate::ecs::kInvalidComponent);

    Watcher on_health;
    Watcher on_mana;
    REQUIRE(world.addObserver(health, &on_health, &Watcher::observe));
    REQUIRE(world.addObserver(mana, &on_mana, &Watcher::observe));

    const Entity entity = world.create();
    REQUIRE(entity.valid());

    SECTION("arriving with a value is a change")
    {
        const Health value{5};
        REQUIRE(world.add(entity, value) != nullptr);
        REQUIRE(on_health.seen.size() == 1U);
        REQUIRE(on_health.seen[0].entity == entity);
        REQUIRE(on_health.seen[0].component == health);
        REQUIRE(on_mana.seen.empty());
    }

    SECTION("a marked write is a change")
    {
        const Mana value{1};
        REQUIRE(world.add(entity, value) != nullptr);

        /* Arriving was announced already; the explicit mark is the second. */
        on_mana.seen.clear();
        world.markChanged(entity, mana);
        REQUIRE(on_mana.seen.size() == 1U);
        REQUIRE(on_mana.seen[0].component == mana);
        REQUIRE(on_health.seen.empty());
    }

    SECTION("a component the entity does not have cannot be marked")
    {
        world.markChanged(entity, health);
        REQUIRE(on_health.seen.empty());
    }
}

TEST_CASE("a removal notifies the observers of the type it leaves", "[ecs][observer]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex health = world.registerComponent<Health>();
    REQUIRE(health != resonate::ecs::kInvalidComponent);

    Watcher watcher;
    REQUIRE(world.addObserver(health, &watcher, &Watcher::observe));

    const Entity entity = world.create();
    const Health value{7};
    REQUIRE(world.add(entity, value) != nullptr);
    REQUIRE(watcher.seen.size() == 1U);

    REQUIRE(world.remove(entity, health));
    REQUIRE(watcher.seen.size() == 2U);
    REQUIRE(watcher.seen[1].entity == entity);
    REQUIRE(watcher.seen[1].component == health);
    REQUIRE_FALSE(world.has(entity, health));
}

TEST_CASE("an observer may not change structure while it runs", "[ecs][observer]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex health = world.registerComponent<Health>();
    const ComponentIndex mana = world.registerComponent<Mana>();
    REQUIRE(health != resonate::ecs::kInvalidComponent);
    REQUIRE(mana != resonate::ecs::kInvalidComponent);

    const Entity target = world.create();
    REQUIRE(target.valid());

    Reentrant reentrant;
    reentrant.target = target;
    reentrant.component = mana;

    Watcher reports;
    world.setReportSink(&reports, &Watcher::sink);
    REQUIRE(world.addObserver(health, &reentrant, &Reentrant::observe));

    const Entity entity = world.create();
    const Health value{1};
    REQUIRE(world.add(entity, value) != nullptr);

    /* The observer ran, its structural call was refused with a report, and the
       refused change did not happen. */
    REQUIRE_FALSE(reentrant.survived);
    REQUIRE(reports.reported("change observer"));
    REQUIRE_FALSE(world.has(target, mana));
}

TEST_CASE("observers of one type run in registration order and can be removed", "[ecs][observer]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex health = world.registerComponent<Health>();
    REQUIRE(health != resonate::ecs::kInvalidComponent);

    std::vector<int> order;
    struct Ordered
    {
        std::vector<int>* order = nullptr;
        int id = 0;

        static void observe(void* user_data, World&, Entity, ComponentIndex)
        {
            auto* self = static_cast<Ordered*>(user_data);
            self->order->push_back(self->id);
        }
    };

    Ordered first{&order, 1};
    Ordered second{&order, 2};
    Ordered third{&order, 3};
    REQUIRE(world.addObserver(health, &first, &Ordered::observe));
    REQUIRE(world.addObserver(health, &second, &Ordered::observe));
    REQUIRE(world.addObserver(health, &third, &Ordered::observe));

    const Entity entity = world.create();
    const Health value{2};
    REQUIRE(world.add(entity, value) != nullptr);
    REQUIRE(order == std::vector<int>{1, 2, 3});

    world.removeObserver(health, &second, &Ordered::observe);
    REQUIRE(world.remove(entity, health));

    REQUIRE(order == std::vector<int>{1, 2, 3, 1, 3});
}

TEST_CASE("playing a command buffer announces the arrivals it applies", "[ecs][observer]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex health = world.registerComponent<Health>();
    REQUIRE(health != resonate::ecs::kInvalidComponent);

    Watcher watcher;
    REQUIRE(world.addObserver(health, &watcher, &Watcher::observe));

    CommandBuffer commands(world);
    const Health value{9};
    const Entity recorded = commands.spawn(value);
    REQUIRE(recorded.valid());

    /* Recorded is not changed: the entity is not even alive until playback. */
    REQUIRE(watcher.seen.empty());
    REQUIRE_FALSE(world.alive(recorded));

    world.play(commands);
    REQUIRE(watcher.seen.size() == 1U);
    REQUIRE(watcher.seen[0].entity == recorded);
    REQUIRE(watcher.seen[0].component == health);
    REQUIRE(world.alive(recorded));

    /* The post-change state is what an observer reads: the value is stored. */
    const Health* stored = world.get<Health>(recorded);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->value == 9);
}

TEST_CASE("an observer for a component this world never registered is refused", "[ecs][observer]")
{
    World world(resonate::systemAllocator());

    Watcher watcher;
    world.setReportSink(&watcher, &Watcher::sink);

    REQUIRE_FALSE(world.addObserver(resonate::ecs::kInvalidComponent, &watcher, &Watcher::observe));
    REQUIRE(watcher.reported("addObserver"));

    const ComponentIndex health = world.registerComponent<Health>();
    REQUIRE(health != resonate::ecs::kInvalidComponent);
    REQUIRE_FALSE(world.addObserver(health, &watcher, nullptr));
    REQUIRE(watcher.reported("is null"));
}
