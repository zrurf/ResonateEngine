#include <catch2/catch_all.hpp>

#include <cstdint>
#include <cstring>

#include "resonate.gameplay/components.gen.h"

namespace
{

/* The layout version 1 data had, as the module that owns the type knows it from
   its own history: the migration entry the schema declares reads exactly this,
   which is why a migration is an owner-provided function rather than generated
   code. */
struct HealthV1
{
    std::int32_t current = 0;
    std::int32_t max = 0;
    std::uint8_t flags = 0;
    float regen = 0.0F;
    std::uint32_t sourceIndex = 0;
    std::uint32_t sourceGeneration = 0;
};

} // namespace

/* The owner's implementation of the declared migration: version 1 payload in,
   current layout out. The generated header declares this function and takes
   its address in kHealthMigrationFrom1, so the entry cannot be used without a
   definition behind it. */
namespace resonate::gameplay
{

bool migrateHealthFrom1(const void* payload, std::size_t size, Health& out)
{
    if (payload == nullptr || size < sizeof(HealthV1))
    {
        return false;
    }

    HealthV1 old;
    std::memcpy(&old, payload, sizeof(old));

    out.current = old.current;
    out.max = old.max;
    out.flags = static_cast<HealthFlags>(old.flags);
    out.regen = old.regen;
    out.source = resonate::ecs::Entity{old.sourceIndex, old.sourceGeneration};
    out.shield = 0; /* the field version 2 added */
    return true;
}

} // namespace resonate::gameplay

TEST_CASE("a declared migration reads the old layout and writes the current one",
          "[schema][migration]")
{
    HealthV1 old;
    old.current = 7;
    old.max = 42;
    old.flags = 2; /* Invulnerable */
    old.regen = 0.25F;
    old.sourceIndex = 3;
    old.sourceGeneration = 1;

    resonate::gameplay::Health now;
    now.current = -1;
    REQUIRE(resonate::gameplay::kHealthMigrationFrom1(&old, sizeof(old), now));
    REQUIRE(now.current == 7);
    REQUIRE(now.max == 42);
    REQUIRE(now.flags == resonate::gameplay::HealthFlags::Invulnerable);
    REQUIRE(now.regen == 0.25F);
    REQUIRE(now.source == (resonate::ecs::Entity{3, 1}));
    REQUIRE(now.shield == 0);

    /* A payload shorter than the old layout is refused rather than read past. */
    REQUIRE_FALSE(resonate::gameplay::kHealthMigrationFrom1(&old, sizeof(HealthV1) - 1, now));

    /* The version the schema declares is what the generated constant carries. */
    REQUIRE(resonate::gameplay::kHealthVersion == 2);
}
