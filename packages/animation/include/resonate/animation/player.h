#ifndef RESONATE_ANIMATION_PLAYER_H
#define RESONATE_ANIMATION_PLAYER_H

/*
 * The animation player: advances every AnimationState by the frame's delta and
 * writes its clip's sample into the entity's LocalTransform. One player owns
 * the run's clips; a state names one by index. A state whose clip is out of
 * range, or whose entity has no LocalTransform, is skipped that frame.
 */

#include <cstdint>
#include <vector>

#include <resonate/animation/clip.h>
#include <resonate/ecs/component.h>
#include <resonate/ecs/world.h>

namespace resonate::animation
{

/* One entity's playback. The player's clip table owns the clip the index
   names, so an id survives states being created and destroyed freely. */
struct AnimationState
{
    std::uint32_t clip = 0U;
    float time = 0.0F;
    /* Time scaling; negative plays backwards. */
    float speed = 1.0F;
    /* When false the state stops at the clip's end (its last key holds). */
    bool looping = true;
    bool playing = true;
};

/* The player runs in the UPDATE stage: sampling reads LocalTransform's source
   and gameplay sees the composed result through the frame's sync points. */
class AnimationPlayer
{
  public:
    /* Registers the state component; idempotent, and a refusal answers false
       like the other domains' registrations. */
    static bool registerComponents(ecs::World& world);

    /* Adds a clip and answers the index states name. */
    std::uint32_t addClip(Clip clip);

    [[nodiscard]] const Clip* clip(std::uint32_t index) const;

    /* The engine-system entry (UPDATE stage). */
    void invoke(ecs::World& world, float delta_seconds);

  private:
    std::vector<Clip> clips_;
};

} // namespace resonate::animation

namespace resonate::ecs
{

/* Runtime data for now: authored clips arrive with the asset slice, and the
   state the player drives is created by code. */
RESONATE_COMPONENT(resonate::animation::AnimationState, "resonate.animation.AnimationState");

} // namespace resonate::ecs

#endif /* RESONATE_ANIMATION_PLAYER_H */
