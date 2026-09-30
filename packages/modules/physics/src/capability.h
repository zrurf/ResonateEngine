#ifndef RESONATE_PHYSICS_CAPABILITY_H
#define RESONATE_PHYSICS_CAPABILITY_H

#include <resonate/physics/world.h>

namespace resonate::physics
{

/* Private module state, allocated from the host. */
struct State
{
    ResonateVec3 gravity{0.0F, -9.81F, 0.0F};
    uint64_t body_count = 0;
};

/* The one world this module publishes. Private to the library. */
ResonatePhysicsWorld& world();

} // namespace resonate::physics

#endif /* RESONATE_PHYSICS_CAPABILITY_H */
