/* The schema declares Health at version 2 with a migration from version 1.
 * This is its implementation: a version-1 payload is the five fields that
 * existed before `shield`, with the same layout the version-2 struct keeps for
 * them. A payload of any other size belongs to no version this build knows. */

#include <cstddef>
#include <cstring>

#include "resonate.gameplay/components.gen.h"

namespace resonate::gameplay
{
namespace
{

struct HealthV1
{
    std::int32_t current;
    std::int32_t max;
    HealthFlags flags;
    float regen;
    ecs::Entity source;
};

static_assert(sizeof(HealthV1) == 24U, "the version-1 layout is five fields");

} // namespace

bool migrateHealthFrom1(const void* payload, std::size_t size, Health& out)
{
    if (payload == nullptr || size != sizeof(HealthV1))
    {
        return false;
    }
    HealthV1 old{};
    std::memcpy(&old, payload, sizeof(old));

    out = Health{};
    out.current = old.current;
    out.max = old.max;
    out.flags = old.flags;
    out.regen = old.regen;
    out.source = old.source;
    return true;
}

} // namespace resonate::gameplay
