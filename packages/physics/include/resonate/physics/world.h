#ifndef RESONATE_PHYSICS_WORLD_H
#define RESONATE_PHYSICS_WORLD_H

#include <resonate/module/capability.h>

#ifdef __cplusplus
#    include <resonate/module/capability_id.hpp>
#endif
#include <resonate/module/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Simulation world: the whole physics surface. */
typedef struct ResonatePhysicsWorld
{
    ResonateCapabilityHeader header;

    /* Fixed-step integration; belongs in RESONATE_STAGE_PHYSICS, which runs single-threaded. */
    ResonateStatus (*step)(void* self, float delta_seconds);

    /* A miss returns RESONATE_OK with out_hit->distance < 0. */
    ResonateStatus (*raycast)(void* self, const ResonateRay* ray, ResonateRayHit* out_hit);

    /* Returns the number of bodies written; fewer than requested is not an error. */
    uint32_t (*overlap)(void* self, const ResonateVec3* center, float radius, uint64_t* out_bodies,
                        uint32_t max_bodies);

    void (*set_gravity)(void* self, ResonateVec3 gravity);

    /* Body handles remain valid for the world's lifetime and are not meaningful
       after this returns. */
    ResonateStatus (*destroy)(void* self);

    /* Provider state; the host never interprets it. */
    void* self;
} ResonatePhysicsWorld;

#ifdef __cplusplus
} // extern "C"

namespace resonate::detail
{
template <> struct CapabilityTraits<ResonatePhysicsWorld>
{
    static constexpr const char* name = "Resonate.Physics.World";
    /* Inline so the whole program shares one definition; not constexpr, because
       the hash is a library call. */
    static inline const Id id{name};
    static constexpr std::uint32_t version = 1;
};
} // namespace resonate::detail
#endif

#endif /* RESONATE_PHYSICS_WORLD_H */
