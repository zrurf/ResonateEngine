#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/blob.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>

namespace
{

using resonate::ecs::BlobHandle;
using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;

/* A hand-written component holding a blob handle, the shape a schema-declared
   blob field generates: the blob itself lives in the store, the component keeps
   the handle. */
struct Tag
{
    BlobHandle blob{};
    std::uint32_t count = 0;
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

RESONATE_COMPONENT(Tag, "test.BlobTag");

} // namespace resonate::ecs

TEST_CASE("a blob holds bytes across creates, resizes and reads", "[ecs][blob]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Tag>() != resonate::ecs::kInvalidComponent);

    const Entity owner = world.create();
    REQUIRE(owner.valid());

    const std::uint32_t seed[4] = {1U, 2U, 3U, 4U};
    const BlobHandle blob = world.createBlob(owner, sizeof(seed), seed);
    REQUIRE(blob.valid());
    REQUIRE(world.alive(blob));
    REQUIRE(world.blobSize(blob) == sizeof(seed));
    REQUIRE(world.blobCount() == 1);
    REQUIRE(std::memcmp(world.blobData(blob), seed, sizeof(seed)) == 0);

    /* Growth keeps the prefix and zeroes the tail. */
    REQUIRE(world.resizeBlob(blob, 7U * sizeof(std::uint32_t)));
    const std::uint32_t* grown = static_cast<const std::uint32_t*>(world.blobData(blob));
    REQUIRE(world.blobSize(blob) == 7U * sizeof(std::uint32_t));
    REQUIRE(std::memcmp(grown, seed, sizeof(seed)) == 0);
    REQUIRE(grown[4] == 0U);
    REQUIRE(grown[6] == 0U);

    /* Shrink keeps the prefix; growth past the shrink zeroes again. */
    REQUIRE(world.resizeBlob(blob, sizeof(std::uint32_t)));
    REQUIRE(world.blobSize(blob) == sizeof(std::uint32_t));
    REQUIRE(*static_cast<const std::uint32_t*>(world.blobData(blob)) == 1U);
    REQUIRE(world.resizeBlob(blob, 2U * sizeof(std::uint32_t)));
    const std::uint32_t* again = static_cast<const std::uint32_t*>(world.blobData(blob));
    REQUIRE(again[0] == 1U);
    REQUIRE(again[1] == 0U);

    /* A zero-size blob is alive and empty, not a stale handle. */
    REQUIRE(world.resizeBlob(blob, 0));
    REQUIRE(world.alive(blob));
    REQUIRE(world.blobSize(blob) == 0);
    REQUIRE(world.blobData(blob) == nullptr);
}

TEST_CASE("a blob handle dies with its blob, not with the next one", "[ecs][blob]")
{
    World world(resonate::systemAllocator());
    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    const Entity owner = world.create();
    const BlobHandle first = world.createBlob(owner, 16);
    REQUIRE(first.valid());
    REQUIRE(world.destroyBlob(first));
    REQUIRE_FALSE(world.alive(first));
    REQUIRE(world.blobCount() == 0);

    /* The freed slot is reused, and the old handle must not read the new blob. */
    const BlobHandle second = world.createBlob(owner, 32);
    REQUIRE(second.valid());
    REQUIRE(second.index == first.index);
    REQUIRE(second.generation != first.generation);
    REQUIRE_FALSE(world.alive(first));
    REQUIRE(world.blobSize(first) == 0);

    REQUIRE(world.blobData(first) == nullptr);
    REQUIRE(reports.contains("blobData: blob"));
}

TEST_CASE("destroying an entity reclaims its blobs and leaves others alone", "[ecs][blob]")
{
    World world(resonate::systemAllocator());

    const Entity first = world.create();
    const Entity second = world.create();
    const BlobHandle one = world.createBlob(first, 8);
    const BlobHandle two = world.createBlob(first, 16);
    const BlobHandle other = world.createBlob(second, 24);
    REQUIRE(world.blobCount() == 3);

    world.destroy(first);
    REQUIRE_FALSE(world.alive(one));
    REQUIRE_FALSE(world.alive(two));
    REQUIRE(world.alive(other));
    REQUIRE(world.blobCount() == 1);
    REQUIRE(world.blobSize(other) == 24);

    /* Destroying the world with live blobs frees them (the sanitizer suite
       checks nothing is left behind). */
    world.destroy(second);
    REQUIRE(world.blobCount() == 0);
}

TEST_CASE("a blob change is refused while parallel execution is in flight", "[ecs][blob]")
{
    World world(resonate::systemAllocator());
    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    const Entity owner = world.create();
    const BlobHandle blob = world.createBlob(owner, 8);
    REQUIRE(blob.valid());

    {
        const World::ParallelScope parallel(world);
        REQUIRE(world.createBlob(owner, 8) == BlobHandle{});
        REQUIRE_FALSE(world.resizeBlob(blob, 64));
        REQUIRE_FALSE(world.destroyBlob(blob));
        REQUIRE(reports.contains("parallel execution is in flight"));
    }

    REQUIRE(world.alive(blob));
    REQUIRE(world.blobSize(blob) == 8);
    REQUIRE(world.resizeBlob(blob, 64));
}

TEST_CASE("a blob needs a live owner", "[ecs][blob]")
{
    World world(resonate::systemAllocator());
    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    REQUIRE(world.createBlob(Entity{}, 8) == BlobHandle{});
    REQUIRE(world.createBlob(Entity{7, 1}, 8) == BlobHandle{});
    REQUIRE(reports.contains("not alive"));
    REQUIRE(world.blobCount() == 0);
}

namespace
{

/* A command a domain defines: appends one number to the blob the entity's Tag
   component names, creating the blob and the component as needed — the shape a
   hierarchy-style recorded edit takes. */
struct AppendOp
{
    Entity entity;
    std::uint32_t value;
};

void playAppend(void*, World& world, const void* payload)
{
    const auto* op = static_cast<const AppendOp*>(payload);
    if (!world.alive(op->entity))
    {
        world.report(World::Report::Error, "playAppend: entity %u:%u is not alive",
                     op->entity.index, op->entity.generation);
        return;
    }

    Tag tag{};
    const Tag* current = world.get<Tag>(op->entity);
    if (current != nullptr)
    {
        tag = *current;
    }
    if (!world.alive(tag.blob))
    {
        tag.blob = world.createBlob(op->entity, 0);
    }

    const std::size_t size = world.blobSize(tag.blob);
    if (!world.resizeBlob(tag.blob, size + sizeof(std::uint32_t)))
    {
        return;
    }
    std::memcpy(static_cast<std::byte*>(world.blobData(tag.blob)) + size, &op->value,
                sizeof(std::uint32_t));
    tag.count += 1;

    if (current != nullptr)
    {
        *world.get<Tag>(op->entity) = tag;
        world.markChanged(op->entity, resonate::ecs::ComponentTraits<Tag>::index);
    }
    else
    {
        world.add(op->entity, tag);
    }
}

} // namespace

TEST_CASE("a recorded command plays in record order with the buffer's own", "[ecs][command]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Tag>() != resonate::ecs::kInvalidComponent);

    CommandBuffer commands(world);
    const Entity entity = commands.create();
    REQUIRE(entity.valid());

    /* The component the play function reads is added by an earlier record, so
       the command order is what makes it visible, and the create before both is
       what makes the entity alive. */
    commands.add<Tag>(entity, Tag{});
    AppendOp op{entity, 0xAABBCCDDU};
    commands.record(&playAppend, nullptr, &op, sizeof(op));
    REQUIRE(commands.commandCount() == 3);

    REQUIRE_FALSE(world.alive(entity));
    world.play(commands);
    REQUIRE(world.alive(entity));
    REQUIRE(commands.empty());

    const Tag* tag = world.get<Tag>(entity);
    REQUIRE(tag != nullptr);
    REQUIRE(tag->count == 1);
    REQUIRE(world.alive(tag->blob));
    REQUIRE(world.blobSize(tag->blob) == sizeof(std::uint32_t));
    std::uint32_t stored = 0;
    std::memcpy(&stored, world.blobData(tag->blob), sizeof(stored));
    REQUIRE(stored == 0xAABBCCDDU);

    /* And again: the second play appends after the first, still finding the
       blob through the component. */
    AppendOp second{entity, 0x55667788U};
    commands.record(&playAppend, nullptr, &second, sizeof(second));
    world.play(commands);
    REQUIRE(world.get<Tag>(entity)->count == 2);
    REQUIRE(world.blobSize(tag->blob) == 2U * sizeof(std::uint32_t));
    std::uint32_t values[2] = {};
    std::memcpy(values, world.blobData(tag->blob), sizeof(values));
    REQUIRE(values[0] == 0xAABBCCDDU);
    REQUIRE(values[1] == 0x55667788U);
}

TEST_CASE("a recorded command that refuses reports through the world", "[ecs][command]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Tag>() != resonate::ecs::kInvalidComponent);
    Reports reports;
    world.setReportSink(&reports, &Reports::sink);

    CommandBuffer commands(world);
    AppendOp op{Entity{99, 1}, 1U};
    commands.record(&playAppend, nullptr, &op, sizeof(op));

    world.play(commands);
    REQUIRE(reports.contains("playAppend: entity 99:1 is not alive"));

    /* A record without a play function is refused at record time. */
    commands.record(nullptr, nullptr, nullptr, 0);
    REQUIRE(commands.empty());
    REQUIRE(reports.contains("a command needs a play function"));
}
