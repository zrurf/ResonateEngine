#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/world.h>

namespace
{

using resonate::ecs::BlobHandle;
using resonate::ecs::ComponentIndex;
using resonate::ecs::Entity;
using resonate::ecs::World;

struct Position
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Label
{
    BlobHandle text{};
};

struct Visit
{
    std::vector<Entity> entities;

    static void entity(void* user_data, Entity entity)
    {
        static_cast<Visit*>(user_data)->entities.push_back(entity);
    }
};

struct BlobVisit
{
    std::vector<BlobHandle> blobs;
    std::vector<Entity> owners;
    std::string first_bytes;

    static void blob(void* user_data, Entity owner, BlobHandle blob, const void* bytes,
                     std::size_t size)
    {
        BlobVisit* self = static_cast<BlobVisit*>(user_data);
        self->owners.push_back(owner);
        self->blobs.push_back(blob);
        if (self->first_bytes.empty() && bytes != nullptr && size != 0U)
        {
            self->first_bytes.assign(static_cast<const char*>(bytes), size);
        }
    }
};

} // namespace

namespace resonate::ecs
{

RESONATE_COMPONENT(Position, "itest.Position");
RESONATE_COMPONENT(Label, "itest.Label");

} // namespace resonate::ecs

TEST_CASE("world: registered types report their name and size", "[ecs][introspection]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    const ComponentIndex label = world.registerComponent<Label>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);
    REQUIRE(label != resonate::ecs::kInvalidComponent);
    CHECK(position != label);

    CHECK(std::strcmp(world.componentName(position), "itest.Position") == 0);
    CHECK(world.componentSize(position) == sizeof(Position));
    CHECK(std::strcmp(world.componentName(label), "itest.Label") == 0);
    CHECK(world.componentSize(label) == sizeof(Label));

    /* A type outside this world's registrations reports nothing. */
    CHECK(world.componentName(resonate::ecs::kInvalidComponent) == nullptr);
    CHECK(world.componentSize(resonate::ecs::kInvalidComponent) == 0U);
    CHECK(world.componentName(static_cast<ComponentIndex>(position + 2U)) == nullptr);
}

TEST_CASE("world: entity and blob walks cover the alive set in slot order", "[ecs][introspection]")
{
    World world(resonate::systemAllocator());
    const ComponentIndex position = world.registerComponent<Position>();
    const ComponentIndex label = world.registerComponent<Label>();
    REQUIRE(position != resonate::ecs::kInvalidComponent);
    REQUIRE(label != resonate::ecs::kInvalidComponent);

    const Entity first = world.create();
    const Entity second = world.create();
    const Entity third = world.create();
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    REQUIRE(third.valid());
    const char text[] = "name";
    Label tagged;
    tagged.text = world.createBlob(second, sizeof(text), text);
    REQUIRE(world.add(second, label, &tagged) != nullptr);
    /* The reserved create is not alive until playback, so it is not walked. */
    world.destroy(third);

    Visit visit;
    world.forEachEntity(&visit, &Visit::entity);
    REQUIRE(visit.entities.size() == 2U);
    CHECK(visit.entities[0] == first);
    CHECK(visit.entities[1] == second);

    BlobVisit blobs;
    world.forEachBlob(&blobs, &BlobVisit::blob);
    REQUIRE(blobs.blobs.size() == 1U);
    CHECK(blobs.blobs[0] == tagged.text);
    CHECK(blobs.owners[0] == second);
    CHECK(blobs.first_bytes == std::string(text, sizeof(text)));
}
