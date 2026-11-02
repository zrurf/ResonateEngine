#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <resonate/application/snapshot.h>
#include <resonate/core/allocator.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/world.h>

namespace
{

using resonate::Span;
using resonate::dom::ComponentDesc;
using resonate::dom::ComponentFieldDesc;
using resonate::ecs::BlobHandle;
using resonate::ecs::ComponentIndex;
using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::save::decodeWorld;
using resonate::save::encodeWorld;
using resonate::save::MigrationRegistry;

struct Link
{
    Entity target{};
    BlobHandle label{};
    float weight = 0.0F;
};

struct Counter
{
    std::uint32_t count = 0U;
    std::int32_t bonus = 0;
};

struct Bare
{
    std::uint32_t value = 0U;
};

/* The version-1 Counter layout: the counter alone. */
bool migrateCounterFrom1(const void* payload, std::size_t size, void* out)
{
    if (payload == nullptr || size != sizeof(std::uint32_t))
    {
        return false;
    }
    auto* migrated = static_cast<Counter*>(out);
    std::memcpy(&migrated->count, payload, sizeof(migrated->count));
    migrated->bonus = 7;
    return true;
}

constexpr ComponentFieldDesc kLinkFields[] = {
    {"target", "entity", offsetof(Link, target), sizeof(Entity), nullptr, 0U},
    {"label", "blob", offsetof(Link, label), sizeof(BlobHandle), nullptr, 0U},
    {"weight", "f32", offsetof(Link, weight), sizeof(float), nullptr, 0U},
};

constexpr ComponentFieldDesc kCounterFields[] = {
    {"count", "u32", offsetof(Counter, count), sizeof(std::uint32_t), nullptr, 0U},
    {"bonus", "i32", offsetof(Counter, bonus), sizeof(std::int32_t), nullptr, 0U},
};

constexpr ComponentFieldDesc kBareFields[] = {
    {"value", "u32", offsetof(Bare, value), sizeof(std::uint32_t), nullptr, 0U},
};

constexpr ComponentDesc kLinkDesc{
    .name = "Link",
    .qualified_name = "stest.Link",
    .size = sizeof(Link),
    .align = alignof(Link),
    .derived = 0U,
    .fields = Span<const ComponentFieldDesc>(kLinkFields),
    .defaults = nullptr,
    .version = 1U,
};

constexpr ComponentDesc kCounterDesc{
    .name = "Counter",
    .qualified_name = "stest.Counter",
    .size = sizeof(Counter),
    .align = alignof(Counter),
    .derived = 0U,
    .fields = Span<const ComponentFieldDesc>(kCounterFields),
    .defaults = nullptr,
    .version = 2U,
};

constexpr ComponentDesc kBareDesc{
    .name = "Bare",
    .qualified_name = "stest.Bare",
    .size = sizeof(Bare),
    .align = alignof(Bare),
    .derived = 0U,
    .fields = Span<const ComponentFieldDesc>(kBareFields),
    .defaults = nullptr,
    .version = 1U,
};

constexpr ComponentDesc kDescs[] = {kLinkDesc, kCounterDesc, kBareDesc};
const resonate::dom::TableComponentRegistry kSchema{Span<const ComponentDesc>(kDescs)};
const MigrationRegistry kNoMigrations{};

/* A payload writer for the migration case: the encoder cannot produce an older
   version's bytes, so the test writes the documented layout itself. */
struct Payload
{
    std::vector<std::byte> bytes;

    void u16(std::uint16_t value)
    {
        for (int byte = 0; byte < 2; ++byte)
        {
            bytes.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
        }
    }

    void u32(std::uint32_t value)
    {
        for (int byte = 0; byte < 4; ++byte)
        {
            bytes.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
        }
    }

    void u8(std::uint8_t value)
    {
        bytes.push_back(std::byte{value});
    }

    void bytesOf(const void* data, std::size_t size)
    {
        const auto* first = static_cast<const std::byte*>(data);
        bytes.insert(bytes.end(), first, first + size);
    }
};

std::vector<Entity> entitiesOf(const World& world)
{
    std::vector<Entity> entities;
    world.forEachEntity(
        &entities,
        [](void* user_data, Entity entity)
        { static_cast<std::vector<Entity>*>(user_data)->push_back(entity); });
    return entities;
}

std::vector<Entity> aliveBlobs(const World& world)
{
    struct Visit
    {
        std::vector<Entity> owners;
        static void sink(void* user_data, Entity owner, BlobHandle, const void*, std::size_t)
        {
            static_cast<Visit*>(user_data)->owners.push_back(owner);
        }
    };
    Visit visit;
    world.forEachBlob(&visit, &Visit::sink);
    return visit.owners;
}

} // namespace

namespace resonate::ecs
{

RESONATE_COMPONENT(Link, "stest.Link");
RESONATE_COMPONENT(Counter, "stest.Counter");
RESONATE_COMPONENT(Bare, "stest.Bare");

} // namespace resonate::ecs

TEST_CASE("snapshot: entities, components and handle fields round trip", "[snapshot]")
{
    World source_world(resonate::systemAllocator());
    REQUIRE(source_world.registerComponent<Link>() != resonate::ecs::kInvalidComponent);
    REQUIRE(source_world.registerComponent<Counter>() != resonate::ecs::kInvalidComponent);
    REQUIRE(source_world.registerComponent<Bare>() != resonate::ecs::kInvalidComponent);

    const Entity first = source_world.create();
    const Entity second = source_world.create();
    const Entity third = source_world.create();
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    REQUIRE(third.valid());
    /* The hole makes the saved slot indices differ from the restored ones, so
       the handle rewrite is observable rather than accidental identity. */
    source_world.destroy(first);

    const char label[] = "linked";
    Link link;
    link.target = third;
    link.label = source_world.createBlob(second, sizeof(label), label);
    link.weight = 0.25F;
    REQUIRE(source_world.add(second, link) != nullptr);
    REQUIRE(link.label.valid());

    Counter counter;
    counter.count = 41U;
    counter.bonus = -3;
    REQUIRE(source_world.add(third, counter) != nullptr);
    Bare bare;
    bare.value = 9U;
    REQUIRE(source_world.add(second, bare) != nullptr);

    std::vector<std::byte> payload;
    std::string error;
    const bool encoded = encodeWorld(source_world, kSchema, payload, error);
    INFO(error);
    REQUIRE(encoded);
    REQUIRE(payload.size() > 16U);

    World restored_world(resonate::systemAllocator());
    REQUIRE(restored_world.registerComponent<Link>() != resonate::ecs::kInvalidComponent);
    REQUIRE(restored_world.registerComponent<Counter>() != resonate::ecs::kInvalidComponent);
    REQUIRE(restored_world.registerComponent<Bare>() != resonate::ecs::kInvalidComponent);
    REQUIRE(decodeWorld(restored_world, kSchema, kNoMigrations,
                Span<const std::byte>(payload.data(), payload.size()), error));

    const std::vector<Entity> before = entitiesOf(source_world);
    const std::vector<Entity> after = entitiesOf(restored_world);
    REQUIRE(before.size() == after.size());
    REQUIRE(before.size() == 2U);
    CHECK(restored_world.entityCount() == source_world.entityCount());

    /* Creation order preserves slot order, so the walks pair up. */
    const Link* restored_link = restored_world.get<Link>(after[0]);
    REQUIRE(restored_link != nullptr);
    CHECK(restored_link->target == after[1]);
    CHECK(restored_link->target.index == 1U);
    CHECK(restored_link->weight == 0.25F);
    REQUIRE(restored_link->label.valid());
    CHECK(restored_world.blobSize(restored_link->label) == sizeof(label));
    CHECK(std::memcmp(restored_world.blobData(restored_link->label), label, sizeof(label)) == 0);

    const Counter* restored_counter = restored_world.get<Counter>(after[1]);
    REQUIRE(restored_counter != nullptr);
    CHECK(restored_counter->count == 41U);
    CHECK(restored_counter->bonus == -3);

    const Bare* restored_bare = restored_world.get<Bare>(after[0]);
    REQUIRE(restored_bare != nullptr);
    CHECK(restored_bare->value == 9U);

    /* The blob belongs to the mapped entity, and the source's handles do not
       survive into the new world. */
    CHECK(aliveBlobs(restored_world) == std::vector<Entity>{after[0]});
}

TEST_CASE("snapshot: an older component runs its migration", "[snapshot]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Counter>() != resonate::ecs::kInvalidComponent);

    Payload payload;
    payload.bytesOf("RSWP", 4U);
    payload.u32(2U); /* snapshot format */
    payload.u32(1U); /* one entity */
    payload.u32(0U); /* no blobs */
    payload.u32(1U); /* one component table entry */
    payload.u16(static_cast<std::uint16_t>(std::strlen("stest.Counter")));
    payload.bytesOf("stest.Counter", std::strlen("stest.Counter"));
    payload.u32(1U); /* written at schema version 1 */
    payload.u8(0U); /* not opaque */
    payload.u32(0U); /* entity index */
    payload.u32(1U); /* generation */
    payload.u32(1U); /* one component */
    payload.u32(0U); /* table entry */
    payload.u32(static_cast<std::uint32_t>(sizeof(std::uint32_t)));
    payload.u32(12U); /* the version-1 count */

    std::string error;
    World without_migration(resonate::systemAllocator());
    REQUIRE(without_migration.registerComponent<Counter>() != resonate::ecs::kInvalidComponent);
    REQUIRE_FALSE(decodeWorld(without_migration, kSchema, kNoMigrations,
                              Span<const std::byte>(payload.bytes.data(), payload.bytes.size()),
                              error));
    INFO(error);
    CHECK(error.find("no migration") != std::string::npos);

    MigrationRegistry migrations;
    REQUIRE(migrations.add("stest.Counter", 1U, &migrateCounterFrom1));
    CHECK_FALSE(migrations.add("stest.Counter", 1U, &migrateCounterFrom1));
    const bool migrated = decodeWorld(world, kSchema, migrations,
                                      Span<const std::byte>(payload.bytes.data(),
                                                            payload.bytes.size()),
                                      error);
    INFO(error);
    REQUIRE(migrated);
    REQUIRE(world.entityCount() == 1U);
    const std::vector<Entity> entities = entitiesOf(world);
    REQUIRE(entities.size() == 1U);
    const Counter* counter = world.get<Counter>(entities[0]);
    REQUIRE(counter != nullptr);
    CHECK(counter->count == 12U);
    CHECK(counter->bonus == 7);
}

TEST_CASE("snapshot: malformed payloads are refused", "[snapshot]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Bare>() != resonate::ecs::kInvalidComponent);
    const std::vector<std::byte> empty;
    std::string error;

    CHECK_FALSE(decodeWorld(world, kSchema, kNoMigrations,
                Span<const std::byte>(empty.data(), empty.size()), error));

    Payload payload;
    payload.bytesOf("RSWP", 4U);
    payload.u32(99U); /* a format this build does not read */
    payload.u32(0U);
    payload.u32(0U);
    payload.u32(0U);
    CHECK_FALSE(decodeWorld(world, kSchema, kNoMigrations,
                Span<const std::byte>(payload.bytes.data(), payload.bytes.size()), error));
    CHECK(error.find("format") != std::string::npos);

    /* A truncated component table. */
    payload.bytes.clear();
    payload.bytesOf("RSWP", 4U);
    payload.u32(2U);
    payload.u32(0U);
    payload.u32(0U);
    payload.u32(1U);
    payload.u16(5U);
    CHECK_FALSE(decodeWorld(world, kSchema, kNoMigrations,
                Span<const std::byte>(payload.bytes.data(), payload.bytes.size()), error));
    CHECK(error.find("truncated") != std::string::npos);

    /* Header counts a payload cannot back: refused, not allocated. */
    payload.bytes.clear();
    payload.bytesOf("RSWP", 4U);
    payload.u32(2U);
    payload.u32(0U);
    payload.u32(0U);
    payload.u32(0xFFFFFFFFU);
    CHECK_FALSE(decodeWorld(world, kSchema, kNoMigrations,
                Span<const std::byte>(payload.bytes.data(), payload.bytes.size()), error));
    CHECK(error.find("truncated") != std::string::npos);

    payload.bytes.clear();
    payload.bytesOf("RSWP", 4U);
    payload.u32(2U);
    payload.u32(1U); /* one entity */
    payload.u32(0U);
    payload.u32(0U);
    payload.u32(0U); /* entity index */
    payload.u32(1U); /* generation */
    payload.u32(0xFFFFFFFFU); /* a component count no payload carries */
    CHECK_FALSE(decodeWorld(world, kSchema, kNoMigrations,
                Span<const std::byte>(payload.bytes.data(), payload.bytes.size()), error));
    CHECK(error.find("truncated") != std::string::npos);
}

TEST_CASE("snapshot: an undescribed component is refused only when carried", "[snapshot]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Bare>() != resonate::ecs::kInvalidComponent);

    const ComponentDesc missing[] = {
        {.name = "stest.Other",
         .qualified_name = "stest.Other",
         .size = sizeof(Bare),
         .align = alignof(Bare),
         .fields = {},
         .version = 1U}};
    const resonate::dom::TableComponentRegistry schema{Span<const ComponentDesc>(missing)};

    std::vector<std::byte> payload;
    std::string error;
    /* A registered type nothing holds does not constrain the save. */
    CHECK(encodeWorld(world, schema, payload, error));

    const Entity entity = world.create();
    REQUIRE(entity.valid());
    Bare bare;
    bare.value = 7U;
    REQUIRE(world.add(entity, bare) != nullptr);
    CHECK_FALSE(encodeWorld(world, schema, payload, error));
    CHECK(error.find("stest.Bare") != std::string::npos);
    CHECK(error.find("descriptor") != std::string::npos);

    /* Declared opaque, the bytes travel without a layout and come back whole. */
    const ComponentIndex opaque[] = {world.findComponent("stest.Bare")};
    REQUIRE(opaque[0] != resonate::ecs::kInvalidComponent);
    REQUIRE(encodeWorld(world, schema, Span<const Entity>(&entity, 1U),
                        Span<const ComponentIndex>(opaque, 1U), payload, error));

    World restored(resonate::systemAllocator());
    REQUIRE(restored.registerComponent<Bare>() != resonate::ecs::kInvalidComponent);
    std::vector<Entity> restored_entities;
    REQUIRE(decodeWorld(restored, schema, kNoMigrations,
                        Span<const std::byte>(payload.data(), payload.size()), error,
                        &restored_entities));
    REQUIRE(restored_entities.size() == 1U);
    const Bare* restored_bare = restored.get<Bare>(restored_entities[0]);
    REQUIRE(restored_bare != nullptr);
    CHECK(restored_bare->value == 7U);
}

TEST_CASE("snapshot: a non-opaque component without a descriptor is refused on decode",
          "[snapshot]")
{
    World world(resonate::systemAllocator());
    REQUIRE(world.registerComponent<Bare>() != resonate::ecs::kInvalidComponent);

    /* A registry that knows Link and Counter but not Bare: the type is
       registered here, its schema is not. */
    const ComponentDesc descs[] = {kLinkDesc, kCounterDesc};
    const resonate::dom::TableComponentRegistry schema{Span<const ComponentDesc>(descs)};

    const auto writeEntry = [&world](Payload& payload, const char* name, std::uint32_t version,
                                     std::uint8_t opaque)
    {
        payload.bytesOf("RSWP", 4U);
        payload.u32(2U); /* snapshot format */
        payload.u32(1U); /* one entity */
        payload.u32(0U); /* no blobs */
        payload.u32(1U); /* one component table entry */
        payload.u16(static_cast<std::uint16_t>(std::strlen(name)));
        payload.bytesOf(name, std::strlen(name));
        payload.u32(version);
        payload.u8(opaque);
        payload.u32(0U); /* entity index */
        payload.u32(1U); /* generation */
        payload.u32(1U); /* one component */
        payload.u32(0U); /* table entry */
        payload.u32(static_cast<std::uint32_t>(sizeof(std::uint32_t)));
        payload.u32(7U); /* the value */
    };

    std::string error;
    Payload payload;
    writeEntry(payload, "stest.Bare", 1U, 0U);
    CHECK_FALSE(decodeWorld(world, schema, kNoMigrations,
                Span<const std::byte>(payload.bytes.data(), payload.bytes.size()), error));
    CHECK(error.find("no schema in this build") != std::string::npos);
    CHECK(world.entityCount() == 0U);

    /* Declared opaque, the same bytes restore verbatim. */
    payload.bytes.clear();
    writeEntry(payload, "stest.Bare", 1U, 1U);
    std::vector<Entity> restored;
    REQUIRE(decodeWorld(world, schema, kNoMigrations,
                        Span<const std::byte>(payload.bytes.data(), payload.bytes.size()), error,
                        &restored));
    REQUIRE(restored.size() == 1U);
    const Bare* bare = world.get<Bare>(restored[0]);
    REQUIRE(bare != nullptr);
    CHECK(bare->value == 7U);
}
