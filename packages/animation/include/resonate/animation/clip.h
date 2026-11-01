#ifndef RESONATE_ANIMATION_CLIP_H
#define RESONATE_ANIMATION_CLIP_H

/*
 * The animation domain's sampling core: a clip is a set of keyframe tracks the
 * sampler walks to produce one entity's local transform. Clips are plain data
 * the player owns; authored clips (a cooked asset format) arrive with the
 * asset slice and ride the same sampling.
 */

#include <cstddef>
#include <string>
#include <string_view>
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
    /* A position track authored `root_motion="true"` is a displacement curve,
       not a pose: it never writes LocalTransform.position; the player
       accumulates its per-frame delta there instead (a walk cycle moves the
       entity, the pose stays in place). */
    bool position_root_motion = false;
    QuatTrack rotation;
    Vec3Track scale;

    /* Samples the tracks at `time` into `transform`. Fields without a track
       keep what they had. Time is taken modulo the duration when the clip
       loops and clamped to it otherwise. A root-motion position track leaves
       the position untouched. */
    void sample(float time, bool looping, hierarchy::LocalTransform& transform) const;

    /* Samples the root-motion position track at a time already inside
       [0, duration]; zero without a root track. */
    [[nodiscard]] Vec3 rootAt(float time) const;

    /* Samples both clips at `time` and blends: position and scale lerp,
       rotation slerps, `weight` 0 answers `from` and 1 answers `to`. A field
       only one clip drives comes from that clip whole; a field neither
       drives keeps what it had. */
    static void blend(const Clip& from, const Clip& to, float weight, float time,
                      bool looping, hierarchy::LocalTransform& transform);

    /* Parses the .rclip XML subset: a <clip duration=".."> root whose children
       are <track field="position|rotation|scale"> elements holding <key
       t=".." value=".."/> keys. The position track may carry
       root_motion="true|false". A value is three floats for the vec3 fields
       and four for rotation, in member order. Keys must be present and
       strictly ascend, a field carries one track at most, and anything the
       grammar does not name is an error with a line number. */
    static bool parse(std::string_view xml, Clip& out, std::string& error);

    /* Reads the file at `path` and parses it; errors carry the path. */
    [[nodiscard]] static bool loadFile(const std::string& path, Clip& out,
                                       std::string& error);

    /* The same blend with each clip sampled at its own time, which is what a
       cross-fade between two states of a machine needs: the outgoing clip is
       where it stopped, the incoming one where it started. */
    static void blend(const Clip& from, const Clip& to, float weight, float from_time,
                      float to_time, bool from_looping, bool to_looping,
                      hierarchy::LocalTransform& transform);
};

} // namespace resonate::animation

#endif /* RESONATE_ANIMATION_CLIP_H */
