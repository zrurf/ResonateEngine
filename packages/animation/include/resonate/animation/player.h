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
#include <resonate/animation/state_machine.h>
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
    /* Time scaling; negative plays backwards. Ignored while `driven`. */
    float speed = 1.0F;
    /* When false the state stops at the clip's end (its last key holds). */
    bool looping = true;
    bool playing = true;
    /* The outgoing side of a cross-fade and the incoming clip's weight; 1 means
       no fade. A machine fills these, a hand-written state leaves them. */
    std::uint32_t previous_clip = 0U;
    float previous_time = 0.0F;
    float blend = 1.0F;
    bool previous_looping = true;
    /* A driver owns the times above: the player samples them without advancing,
       so a machine's clock is the only one running. */
    bool driven = false;
};

/* One entity's state machine: an index into the player's machine table and the
   live runtime. Gameplay writes its parameters; the player advances it and
   folds the result into the entity's AnimationState, which gameplay must
   attach too. */
struct MachineState
{
    std::uint32_t machine = 0U;
    MachineRuntime runtime;
};

/* The player runs in the UPDATE stage: sampling reads LocalTransform's source
   and gameplay sees the composed result through the frame's sync points. */
class AnimationPlayer
{
  public:
    /* Registers the state components; idempotent, and a refusal answers false
       like the other domains' registrations. */
    static bool registerComponents(ecs::World& world);

    /* Adds a clip and answers the index states name. */
    std::uint32_t addClip(Clip clip);

    [[nodiscard]] const Clip* clip(std::uint32_t index) const;

    /* Adds a machine and answers the index MachineState names. */
    std::uint32_t addMachine(StateMachine machine);

    [[nodiscard]] const StateMachine* machine(std::uint32_t index) const;

    /* The engine-system entry (UPDATE stage). */
    void invoke(ecs::World& world, float delta_seconds);

  private:
    /* Advances every MachineState and writes its AnimationState. */
    void advanceMachines(ecs::World& world, float delta_seconds);

    std::vector<Clip> clips_;
    std::vector<StateMachine> machines_;
};

} // namespace resonate::animation

namespace resonate::ecs
{

/* Runtime data for now: authored clips arrive with the asset slice, and the
   state the player drives is created by code. */
RESONATE_COMPONENT(resonate::animation::AnimationState, "resonate.animation.AnimationState");
RESONATE_COMPONENT(resonate::animation::MachineState, "resonate.animation.MachineState");

} // namespace resonate::ecs

#endif /* RESONATE_ANIMATION_PLAYER_H */
