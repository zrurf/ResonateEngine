#include "capability.h"

namespace resonate::physics
{
namespace
{

ResonateStatus step(void* self, float)
{
    return self != nullptr ? RESONATE_OK : RESONATE_E_STATE;
}

ResonateStatus raycast(void* self, const ResonateRay*, ResonateRayHit* out_hit)
{
    if (self == nullptr || out_hit == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    out_hit->distance = -1.0F;
    return RESONATE_OK;
}

uint32_t overlap(void*, const ResonateVec3*, float, uint64_t*, uint32_t)
{
    return 0;
}

void setGravity(void* self, ResonateVec3 gravity)
{
    if (auto* state = static_cast<State*>(self))
    {
        state->gravity = gravity;
    }
}

ResonateStatus destroy(void* self)
{
    return self != nullptr ? RESONATE_OK : RESONATE_E_STATE;
}

ResonatePhysicsWorld g_world = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonatePhysicsWorld, RESONATE_THREAD_MAIN),
    &step,
    &raycast,
    &overlap,
    &setGravity,
    &destroy,
    nullptr,
};

} // namespace

ResonatePhysicsWorld& world()
{
    return g_world;
}

} // namespace resonate::physics
