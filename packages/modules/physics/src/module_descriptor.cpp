#include <resonate/physics/world.h>

#include <resonate/module/module_descriptor.hpp>

RESONATE_MODULE_DESCRIPTOR("resonate.physics", "Physics", "0.1.0", 1,
                           RESONATE_CAPABILITIES(ResonatePhysicsWorld), RESONATE_CAPABILITIES(),
                           RESONATE_CAPABILITIES(), RESONATE_MODULE_NAMES())
