#ifndef RESONATE_ANIMATION_CLIP_H
#define RESONATE_ANIMATION_CLIP_H

/*
 * The animation domain's sampling core: a clip is a set of keyframe tracks the
 * sampler walks to produce one entity's local transform. Clips are plain data
 * the player owns; authored clips (a cooked asset format) arrive with the
 * asset slice and ride the same sampling.
 */

#include <cstddef>
#include <vector>

#include <resonate/core/math.h>
#include "resonate.hierarchy/components.gen.h"

namespace resonate::animation
{

/* One channel: samples sorted by time, the first at or before the sample
   time and the next one lerp (or slerp) between. A track with a single key
   holds that value for the whole clip; an empty track leaves the field it
   drives untouched. */
struct Vec3Track
{
    struct Key
    {
        float time = 0.0F;
        Vec3 value{};
    };

    std::vector<Key> keys;
};

struct QuatTrack
{
    struct Key
    {
        float time = 0.0F;
        Quat value{};
    };

    std::vector<Key> keys;
};

struct Clip
{
    /* The loop the player wraps at; tracks may end earlier (they hold their
       last value to the end) but not later. */
    float duration = 0.0F;
    Vec3Track position;
    QuatTrack rotation;
    Vec3Track scale;

    /* Samples the tracks at `time` into `transform`. Fields without a track
       keep what they had. Time is taken modulo the duration when the clip
       loops and clamped to it otherwise. */
    void sample(float time, bool looping, hierarchy::LocalTransform& transform) const;
};

} // namespace resonate::animation

#endif /* RESONATE_ANIMATION_CLIP_H */
