#include <catch2/catch_all.hpp>

#include <string>

#include <resonate/core/allocator.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/name.h>
#include <resonate/hierarchy/tree.h>

namespace
{

using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;

} // namespace

TEST_CASE("an entity's display name round-trips through the blob store", "[hierarchy][name]")
{
    World world(resonate::systemAllocator());
    CommandBuffer commands(world);
    REQUIRE(resonate::hierarchy::registerComponents(world));

    const Entity entity = commands.create();
    world.play(commands);
    REQUIRE(world.alive(entity));

    /* Unnamed: no component, empty answer. */
    CHECK(resonate::hierarchy::nameOf(world, entity).empty());
    std::string error;
    REQUIRE(resonate::hierarchy::setName(world, entity, "箱子 A", error));
    CHECK(error.empty());
    CHECK(resonate::hierarchy::nameOf(world, entity) == "箱子 A");

    /* A rename replaces the blob; the old bytes are gone with it. */
    REQUIRE(resonate::hierarchy::setName(world, entity, "crate", error));
    CHECK(resonate::hierarchy::nameOf(world, entity) == "crate");
    const auto* name = world.get<resonate::hierarchy::Name>(entity);
    REQUIRE(name != nullptr);
    REQUIRE(world.alive(name->text));
    CHECK(world.blobSize(name->text) == 5U);

    /* An empty name clears the bytes and keeps the component. */
    REQUIRE(resonate::hierarchy::setName(world, entity, "", error));
    CHECK(resonate::hierarchy::nameOf(world, entity).empty());
    CHECK(world.get<resonate::hierarchy::Name>(entity) != nullptr);

    /* A second entity keeps its own bytes. */
    const Entity other = commands.create();
    world.play(commands);
    REQUIRE(resonate::hierarchy::setName(world, other, "other", error));
    CHECK(resonate::hierarchy::nameOf(world, entity).empty());
    CHECK(resonate::hierarchy::nameOf(world, other) == "other");
}

TEST_CASE("a name on a dead entity is refused", "[hierarchy][name]")
{
    World world(resonate::systemAllocator());
    CommandBuffer commands(world);
    REQUIRE(resonate::hierarchy::registerComponents(world));

    const Entity entity = commands.create();
    resonate::hierarchy::destroyEntity(commands, entity);
    world.play(commands);

    std::string error;
    CHECK_FALSE(resonate::hierarchy::setName(world, entity, "ghost", error));
    CHECK_FALSE(error.empty());
}
