#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/core/job.h>
#include <resonate/core/math.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/system.h>
#include <resonate/hierarchy/tree.h>

namespace
{

using resonate::JobSystem;
using resonate::Mat4;
using resonate::Vec3;
using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;
using namespace resonate::hierarchy;

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

/* A world with the tree registered and one buffer to record into. */
struct Scene
{
    World world;
    Reports reports;
    CommandBuffer commands;

    Scene() : world(resonate::systemAllocator()), commands(world)
    {
        world.setReportSink(&reports, &Reports::sink);
        REQUIRE(registerComponents(world));
    }

    [[nodiscard]] Entity node(Entity parent, Vec3 position)
    {
        const Entity entity = commands.create();
        LocalTransform local;
        local.position = position;
        local.rotation = resonate::Quat{};
        local.scale = Vec3{1.0F, 1.0F, 1.0F};
        commands.add<LocalTransform>(entity, local);
        commands.add<WorldTransform>(entity, WorldTransform{});
        if (parent.valid())
        {
            setParent(commands, entity, parent);
        }
        return entity;
    }
};

bool nearlyEqual(const Vec3& left, const Vec3& right)
{
    const auto close = [](float a, float b) { return a - b < 1.0e-4F && b - a < 1.0e-4F; };
    return close(left.x, right.x) && close(left.y, right.y) && close(left.z, right.z);
}

Vec3 translationOf(World& world, Entity entity)
{
    const WorldTransform* transform = world.get<WorldTransform>(entity);
    REQUIRE(transform != nullptr);
    return Vec3{transform->matrix.m[0][3], transform->matrix.m[1][3], transform->matrix.m[2][3]};
}

} // namespace

TEST_CASE("world transforms compose down the tree", "[hierarchy][propagation]")
{
    Scene scene;
    const Entity root = scene.node(Entity{}, Vec3{1.0F, 0.0F, 0.0F});
    const Entity middle = scene.node(root, Vec3{0.0F, 2.0F, 0.0F});
    const Entity leaf = scene.node(middle, Vec3{0.0F, 0.0F, 3.0F});
    const Entity other = scene.node(Entity{}, Vec3{5.0F, 5.0F, 5.0F});
    scene.world.play(scene.commands);

    PropagationSystem propagation;
    propagation.run(scene.world);

    REQUIRE(nearlyEqual(translationOf(scene.world, root), Vec3{1.0F, 0.0F, 0.0F}));
    REQUIRE(nearlyEqual(translationOf(scene.world, middle), Vec3{1.0F, 2.0F, 0.0F}));
    REQUIRE(nearlyEqual(translationOf(scene.world, leaf), Vec3{1.0F, 2.0F, 3.0F}));
    REQUIRE(nearlyEqual(translationOf(scene.world, other), Vec3{5.0F, 5.0F, 5.0F}));

    /* A reparent recorded this frame is composed by the next run: the leaf's
       local (0, 0, 3) lands under the other root's (5, 5, 5). */
    setParent(scene.commands, leaf, other);
    scene.world.play(scene.commands);
    propagation.run(scene.world);
    REQUIRE(nearlyEqual(translationOf(scene.world, leaf), Vec3{5.0F, 5.0F, 8.0F}));
}

TEST_CASE("a rotation and a scale reach the descendants", "[hierarchy][propagation]")
{
    Scene scene;
    const Entity parent = scene.node(Entity{}, Vec3{10.0F, 0.0F, 0.0F});
    const Entity child = scene.node(parent, Vec3{1.0F, 0.0F, 0.0F});
    scene.world.play(scene.commands);

    /* The parent turns a quarter turn about +Z and doubles its scale: the child
       at +X one unit out ends up two units along +Y. */
    LocalTransform* local = scene.world.get<LocalTransform>(parent);
    REQUIRE(local != nullptr);
    local->rotation = resonate::Quat{0.0F, 0.0F, 0.70710678F, 0.70710678F};
    local->scale = Vec3{2.0F, 2.0F, 2.0F};

    PropagationSystem propagation;
    propagation.run(scene.world);
    REQUIRE(nearlyEqual(translationOf(scene.world, child), Vec3{10.0F, 2.0F, 0.0F}));
}

TEST_CASE("a node whose parent is not a transform node becomes its own root",
          "[hierarchy][propagation]")
{
    Scene scene;
    const Entity plain = scene.commands.create(); /* no transform components */
    const Entity child = scene.node(plain, Vec3{4.0F, 0.0F, 0.0F});
    scene.world.play(scene.commands);
    REQUIRE(scene.world.alive(plain));

    PropagationSystem propagation;
    propagation.run(scene.world);
    REQUIRE(nearlyEqual(translationOf(scene.world, child), Vec3{4.0F, 0.0F, 0.0F}));
}

TEST_CASE("a deep chain propagates to the last level", "[hierarchy][propagation]")
{
    Scene scene;
    std::vector<Entity> chain;
    Entity previous{};
    for (std::uint32_t depth = 0; depth <= kMaxDepth; ++depth)
    {
        const Entity node = scene.node(previous, Vec3{0.0F, 1.0F, 0.0F});
        chain.push_back(node);
        previous = node;
    }
    scene.world.play(scene.commands);

    PropagationSystem propagation;
    propagation.run(scene.world);

    for (std::uint32_t depth = 0; depth <= kMaxDepth; ++depth)
    {
        const float expected = static_cast<float>(depth) + 1.0F;
        REQUIRE(nearlyEqual(translationOf(scene.world, chain[depth]), Vec3{0.0F, expected, 0.0F}));
    }
}

TEST_CASE("nodes the walk cannot reach are reported, not silently skipped",
          "[hierarchy][propagation]")
{
    Scene scene;
    const Entity parent = scene.node(Entity{}, Vec3{1.0F, 0.0F, 0.0F});
    const Entity child = scene.node(parent, Vec3{0.0F, 1.0F, 0.0F});
    const Entity other = scene.node(Entity{}, Vec3{2.0F, 0.0F, 0.0F});
    const Entity doomed = scene.node(other, Vec3{0.0F, 0.0F, 1.0F});
    scene.world.play(scene.commands);

    /* Corrupt the first parent's list by hand: the child's Parent points at it,
       but the list is gone. The walk must notice instead of treating the child
       as a root with a stale transform. */
    const Children* kids = scene.world.get<Children>(parent);
    REQUIRE(kids != nullptr);
    REQUIRE(scene.world.destroyBlob(kids->children));
    REQUIRE(scene.world.remove<Children>(parent));

    /* And a plain destroy leaves a dead handle in the other list, which is the
       other thing worth a report. */
    scene.world.destroy(doomed);

    PropagationSystem propagation;
    propagation.run(scene.world);

    REQUIRE(scene.reports.contains("unreachable from any root"));
    REQUIRE(scene.reports.contains("stale child handle"));
    /* The unreachable child keeps whatever transform it had: the walk skipped
       it rather than guessing a parent. */
    REQUIRE(scene.world.alive(child));
    REQUIRE(nearlyEqual(translationOf(scene.world, parent), Vec3{1.0F, 0.0F, 0.0F}));
    REQUIRE(nearlyEqual(translationOf(scene.world, other), Vec3{2.0F, 0.0F, 0.0F}));
}

TEST_CASE("the levels run over the job pool", "[hierarchy][propagation]")
{
    Scene scene;
    const Entity root = scene.node(Entity{}, Vec3{0.0F, 1.0F, 0.0F});
    std::vector<Entity> children;
    for (int index = 0; index < 500; ++index)
    {
        children.push_back(scene.node(root, Vec3{1.0F, 0.0F, 0.0F}));
    }
    scene.world.play(scene.commands);

    JobSystem* jobs = resonate::createJobSystem();
    REQUIRE(jobs != nullptr);

    PropagationSystem propagation;
    propagation.jobs = jobs;
    propagation.run(scene.world);

    for (const Entity child : children)
    {
        REQUIRE(nearlyEqual(translationOf(scene.world, child), Vec3{1.0F, 1.0F, 0.0F}));
    }
    REQUIRE(nearlyEqual(translationOf(scene.world, root), Vec3{0.0F, 1.0F, 0.0F}));

    delete jobs;
}

TEST_CASE("ten thousand nodes propagate inside the frame budget",
          "[hierarchy][propagation][benchmark]")
{
    Scene scene;
    const Entity root = scene.node(Entity{}, Vec3{1.0F, 0.0F, 0.0F});
    std::vector<Entity> parents;
    parents.push_back(root);

    /* A wide-and-deep mix: four levels of ten children each, 11111 nodes. */
    for (int level = 0; level < 4; ++level)
    {
        std::vector<Entity> next;
        for (const Entity parent : parents)
        {
            for (int index = 0; index < 10; ++index)
            {
                next.push_back(scene.node(parent, Vec3{0.0F, 1.0F, 0.0F}));
            }
        }
        parents = next;
    }
    scene.world.play(scene.commands);
    REQUIRE(scene.world.entityCount() > 10000);

    JobSystem* jobs = resonate::createJobSystem();
    REQUIRE(jobs != nullptr);
    PropagationSystem propagation;
    propagation.jobs = jobs;

    const auto started = std::chrono::steady_clock::now();
    propagation.run(scene.world);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const double milliseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1000.0;
    std::printf("[hierarchy] %u nodes propagated in %.2f ms\n", scene.world.entityCount(),
                milliseconds);

    /* A floor that fails when a change turns the walk into something
       quadratic, not when a machine is slow. */
    REQUIRE(milliseconds < 200.0);

    const auto listed = children(scene.world, root);
    REQUIRE(listed.size() == 10);
    REQUIRE(nearlyEqual(translationOf(scene.world, listed[0]), Vec3{1.0F, 1.0F, 0.0F}));

    delete jobs;
}
