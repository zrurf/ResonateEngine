#include <resonate/animation/clip.h>

#include <algorithm>
#include <cmath>

namespace resonate::animation
{
namespace
{

/* The segment [begin, end) that brackets `time`: the keys are sorted, so a
   linear walk is fine for the key counts a clip carries. Answers the pair to
   interpolate and the 0..1 weight inside it; outside the keys the weight
   pins to the nearest end. */
template <typename Track>
bool bracket(const Track& track, float time, std::size_t& index, float& alpha)
{
    if (track.keys.size() < 2U || time <= track.keys.front().time)
    {
        return false;
    }
    if (time >= track.keys.back().time)
    {
        index = track.keys.size() - 2U;
        alpha = 1.0F;
        return true;
    }
    for (std::size_t i = 0U; i + 1U < track.keys.size(); ++i)
    {
        if (time < track.keys[i + 1U].time)
        {
            index = i;
            const float span = track.keys[i + 1U].time - track.keys[i].time;
            alpha = span > 0.0F ? (time - track.keys[i].time) / span : 0.0F;
            return true;
        }
    }
    return false;
}

Vec3 sampleVec3(const Vec3Track& track, float time)
{
    std::size_t index = 0U;
    float alpha = 0.0F;
    if (!bracket(track, time, index, alpha))
    {
        return track.keys.empty() ? Vec3{} : track.keys.front().value;
    }
    const Vec3Track::Key& from = track.keys[index];
    const Vec3Track::Key& to = track.keys[index + 1U];
    return from.value + (to.value - from.value) * alpha;
}

Quat sampleQuat(const QuatTrack& track, float time)
{
    std::size_t index = 0U;
    float alpha = 0.0F;
    if (!bracket(track, time, index, alpha))
    {
        return track.keys.empty() ? Quat{} : track.keys.front().value;
    }
    return slerp(track.keys[index].value, track.keys[index + 1U].value, alpha);
}

} // namespace

void Clip::sample(float time, bool looping, hierarchy::LocalTransform& transform) const
{
    float within = time;
    if (looping && duration > 0.0F)
    {
        within = std::fmod(time, duration);
        if (within < 0.0F)
        {
            within += duration;
        }
    }
    else if (time < 0.0F)
    {
        within = 0.0F;
    }
    else if (duration > 0.0F && time > duration)
    {
        within = duration;
    }

    if (!position.keys.empty())
    {
        transform.position = sampleVec3(position, within);
    }
    if (!rotation.keys.empty())
    {
        transform.rotation = sampleQuat(rotation, within);
    }
    if (!scale.keys.empty())
    {
        transform.scale = sampleVec3(scale, within);
    }
}

} // namespace resonate::animation
