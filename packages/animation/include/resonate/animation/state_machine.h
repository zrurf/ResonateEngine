#ifndef RESONATE_ANIMATION_STATE_MACHINE_H
#define RESONATE_ANIMATION_STATE_MACHINE_H

/*
 * A state machine over clips: states name a clip, edges carry conditions on
 * named parameters, and taking an edge cross-fades. The definition is data; a
 * runtime holds the live state; advance() is a pure function over both, so a
 * replay can re-run the same ticks and land in the same state.
 *
 * The shape follows Godot's AnimationNodeStateMachine: a transition is taken
 * when its every condition holds, the lowest priority wins, ties break by
 * declaration order, and an edge marked sync inherits the source's normalized
 * time.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace resonate::animation
{

/* An edge with this `from` matches whichever state is current. */
inline constexpr std::uint16_t kAnyState = 0xFFFFU;
/* Values and triggers a runtime holds; a condition's parameter must stay below
   this bound, which validate() checks. */
inline constexpr std::uint32_t kMaxMachineParameters = 16U;
inline constexpr std::uint32_t kUnknownParameter = 0xFFFFFFFFU;

enum class ConditionTest : std::uint8_t
{
    Greater,
    Less,
    Equal,
    NotEqual,
    True,
    False,
    /* Set for a frame by the host and cleared by clearTriggers(); never
       cleared by evaluation, so several edges can see the same trigger. */
    Triggered,
};

struct Condition
{
    std::uint32_t parameter = 0U;
    ConditionTest test = ConditionTest::True;
    /* The right-hand side of a comparison; ignored by True/False/Triggered. */
    float value = 0.0F;
};

struct Transition
{
    std::uint16_t from = kAnyState;
    std::uint16_t to = 0U;
    std::vector<Condition> conditions;
    /* Cross-fade length; 0 switches on the frame the edge is taken. */
    float fade_seconds = 0.15F;
    std::int32_t priority = 0;
    /* Only considered once the current state's non-looping clip reached its
       end. */
    bool at_end_only = false;
    /* Start the target at the source's normalized time, so a walk cycle does
       not restart when switching to a run cycle. Needs both durations. */
    bool sync = false;
};

struct State
{
    std::uint32_t clip = 0U;
    /* The clip's duration, copied when the machine is built: the machine wraps
       and syncs without the clip table. 0 leaves the state time unbounded. */
    float duration = 0.0F;
    bool looping = true;
};

struct StateMachine
{
    std::vector<State> states;
    std::vector<Transition> transitions;
    std::vector<std::string> parameters;
    std::uint16_t initial = 0U;

    /* The parameter index for `name`, or kUnknownParameter. */
    [[nodiscard]] std::uint32_t parameterIndex(std::string_view name) const;

    /* Answers false with `error` when a state, transition or parameter index is
       out of range, so a loaded machine fails at build time instead of
       silently never transitioning. */
    [[nodiscard]] bool validate(std::string& error) const;
};

/* One entity's live machine. Values and triggers are indices into the
   definition's parameter table. */
struct MachineRuntime
{
    std::uint16_t current = 0U;
    std::uint16_t previous = 0U;
    float time = 0.0F;
    float previous_time = 0.0F;
    float fade_remaining = 0.0F;
    float fade_duration = 0.0F;
    float values[kMaxMachineParameters] = {};
    std::uint32_t triggers = 0U;
    /* False until the first advance() enters the initial state. */
    bool started = false;
    /* The same clock without the loop wrap: root motion reads it so a frame
       that crosses the clip's end contributes the tail it passed. */
    float raw_time = 0.0F;

    void setValue(std::uint32_t parameter, float value);
    void setTrigger(std::uint32_t parameter);
    void clearTriggers();
    [[nodiscard]] float value(std::uint32_t parameter) const;
    [[nodiscard]] bool triggered(std::uint32_t parameter) const;
};

/* The weight of the current state: 1 without a fade, otherwise the fraction of
   the fade that has elapsed. `previous`/`previous_time` are the other side
   while this is below 1. */
[[nodiscard]] float blendWeight(const MachineRuntime& runtime);

[[nodiscard]] bool conditionHolds(const Condition& condition, const MachineRuntime& runtime);

/* Advances one frame: moves the state time (wrapping on a looping state,
   clamping on a held one), decays the fade, then takes the best qualifying
   edge and starts its fade. Answers the edge taken, or nullptr. */
const Transition* advance(const StateMachine& machine, MachineRuntime& runtime,
                          float delta_seconds);

} // namespace resonate::animation

#endif /* RESONATE_ANIMATION_STATE_MACHINE_H */
