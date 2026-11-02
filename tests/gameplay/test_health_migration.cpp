#include <catch2/catch_all.hpp>

#include <cstdint>
#include <cstring>

#include "resonate.gameplay/nodes.gen.h"

namespace
{

/* The version-1 Health layout the migration was written against: the five
   fields that existed before `shield`. */
struct HealthV1
{
    std::int32_t current;
    std::int32_t max;
    resonate::gameplay::HealthFlags flags;
    float regen;
    resonate::ecs::Entity source;
};

static_assert(sizeof(HealthV1) == 24U, "the version-1 layout is five fields");

} // namespace

TEST_CASE("gameplay: the schema's Health migration writes the current layout", "[gameplay]")
{
    HealthV1 old{};
    old.current = 7;
    old.max = 30;
    old.flags = resonate::gameplay::HealthFlags::Poisoned;
    old.regen = 0.75F;
    old.source = resonate::ecs::Entity{3U, 9U};

    resonate::gameplay::Health migrated{};
    migrated.shield = 5;
    REQUIRE(resonate::gameplay::generated::kHealthMigrateFrom1(&old, sizeof(old), &migrated));
    CHECK(migrated.current == 7);
    CHECK(migrated.max == 30);
    CHECK(migrated.flags == resonate::gameplay::HealthFlags::Poisoned);
    CHECK(migrated.regen == 0.75F);
    CHECK(migrated.source == old.source);
    CHECK(migrated.shield == 0);

    /* A payload that is not the version-1 shape is refused, not reinterpreted. */
    CHECK_FALSE(resonate::gameplay::generated::kHealthMigrateFrom1(&old, 4U, &migrated));
    CHECK_FALSE(
        resonate::gameplay::generated::kHealthMigrateFrom1(nullptr, sizeof(old), &migrated));
}
