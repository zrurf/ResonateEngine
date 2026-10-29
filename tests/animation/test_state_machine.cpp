#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

#include <resonate/animation/state_machine.h>

using resonate::animation::advance;
using resonate::animation::blendWeight;
using resonate::animation::Condition;
using resonate::animation::ConditionTest;
using resonate::animation::kAnyState;
using resonate::animation::MachineRuntime;
using resonate::animation::State;
using resonate::animation::StateMachine;
using resonate::animation::Transition;

namespace
{

/* idle (looping, 2 s) <-> walk (looping, 1 s) on `speed`, plus a triggered
   one-shot jump (1 s, held) that lands back in idle. */
StateMachine makeMachine()
{
    StateMachine machine;
    machine.parameters = {"speed", "jump"};
    machine.states.push_back(State{0U, 2.0F, true});
    machine.states.push_back(State{1U, 1.0F, true});
    machine.states.push_back(State{2U, 1.0F, false});
    machine.initial = 0U;

    Transition walk;
    walk.from = 0U;
    walk.to = 1U;
    walk.conditions.push_back(Condition{0U, ConditionTest::Greater, 0.1F});
    walk.fade_seconds = 0.25F;
    walk.priority = 1;
    machine.transitions.push_back(walk);

    Transition stop;
    stop.from = 1U;
    stop.to = 0U;
    stop.conditions.push_back(Condition{0U, ConditionTest::Less, 0.1F});
    stop.fade_seconds = 0.0F;
    machine.transitions.push_back(stop);

    /* The any-state jump outranks the walk edge, so it wins on the frame both
       qualify. */
    Transition jump;
    jump.from = kAnyState;
    jump.to = 2U;
    jump.conditions.push_back(Condition{1U, ConditionTest::Triggered, 0.0F});
    jump.fade_seconds = 0.0F;
    jump.priority = 0;
    machine.transitions.push_back(jump);

    Transition land;
    land.from = 2U;
    land.to = 0U;
    land.at_end_only = true;
    land.fade_seconds = 0.1F;
    machine.transitions.push_back(land);
    return machine;
}

} // namespace

TEST_CASE("animation state machine: the definition validates its indices", "[animation]")
{
    StateMachine machine = makeMachine();
    std::string error;
    CHECK(machine.validate(error));

    StateMachine empty;
    CHECK_FALSE(empty.validate(error));

    StateMachine bad_target = makeMachine();
    bad_target.transitions[0].to = 7U;
    error.clear();
    CHECK_FALSE(bad_target.validate(error));
    CHECK(error.find("unknown state") != std::string::npos);

    StateMachine bad_parameter = makeMachine();
    bad_parameter.transitions[0].conditions[0].parameter = 3U;
    CHECK_FALSE(bad_parameter.validate(error));

    CHECK(machine.parameterIndex("speed") == 0U);
    CHECK(machine.parameterIndex("jump") == 1U);
    CHECK(machine.parameterIndex("missing") == resonate::animation::kUnknownParameter);
}

TEST_CASE("animation state machine: edges fire on parameters and fade in", "[animation]")
{
    StateMachine machine = makeMachine();
    std::string error;
    REQUIRE(machine.validate(error));
    MachineRuntime runtime;

    /* The first advance enters the initial state and takes no edge: speed is
       still zero and only the walk edge leaves idle. */
    advance(machine, runtime, 0.0F);
    CHECK(runtime.started);
    CHECK(runtime.current == 0U);
    CHECK(runtime.fade_duration == 0.0F);

    runtime.setValue(0U, 1.0F);
    const Transition* taken = advance(machine, runtime, 0.016F);
    REQUIRE(taken != nullptr);
    CHECK(taken->to == 1U);
    CHECK(runtime.current == 1U);
    CHECK(runtime.previous == 0U);
    /* The quarter-second fade starts at full length, so the weight is 0 now. */
    CHECK(std::fabs(runtime.fade_remaining - 0.25F) < 0.0001F);
    CHECK(std::fabs(blendWeight(runtime)) < 0.0001F);

    /* Halfway through the fade the weight is the elapsed fraction. */
    advance(machine, runtime, 0.125F);
    CHECK(std::fabs(blendWeight(runtime) - 0.5F) < 0.001F);
    advance(machine, runtime, 0.2F);
    CHECK(blendWeight(runtime) == 1.0F);
    CHECK(runtime.previous == runtime.current);
}

TEST_CASE("animation state machine: a trigger outranks a parameter edge", "[animation]")
{
    StateMachine machine = makeMachine();
    MachineRuntime runtime;
    runtime.setValue(0U, 1.0F);
    runtime.setTrigger(1U);
    advance(machine, runtime, 0.0F);
    /* Both the walk edge (priority 1) and the jump edge (priority 0) qualify;
       the lower priority wins even though the walk edge is declared first. */
    CHECK(runtime.current == 2U);

    /* The trigger stays readable until the host clears it. */
    CHECK(runtime.triggered(1U));
    runtime.clearTriggers();
    CHECK_FALSE(runtime.triggered(1U));
}

TEST_CASE("animation state machine: an at-end edge waits for a held clip", "[animation]")
{
    StateMachine machine = makeMachine();
    MachineRuntime runtime;
    runtime.setValue(0U, 1.0F);
    runtime.setTrigger(1U);
    advance(machine, runtime, 0.0F);
    REQUIRE(runtime.current == 2U);
    runtime.clearTriggers();

    /* The jump clip is one second and does not loop: half way through, the
       at-end edge into idle must not fire. */
    advance(machine, runtime, 0.5F);
    CHECK(runtime.current == 2U);
    CHECK(std::fabs(runtime.time - 0.5F) < 0.0001F);

    advance(machine, runtime, 0.6F);
    CHECK(runtime.current == 0U);
    CHECK(std::fabs(runtime.fade_remaining - 0.1F) < 0.0001F);
}

TEST_CASE("animation state machine: a looping state wraps and never ends", "[animation]")
{
    StateMachine machine = makeMachine();
    MachineRuntime runtime;
    runtime.setValue(0U, 1.0F);
    advance(machine, runtime, 0.0F);
    REQUIRE(runtime.current == 1U);

    advance(machine, runtime, 2.5F);
    CHECK(runtime.current == 1U);
    CHECK(std::fabs(runtime.time - 0.5F) < 0.0001F);
}

TEST_CASE("animation state machine: a synced edge keeps the cycle phase", "[animation]")
{
    StateMachine machine = makeMachine();
    for (Transition& transition : machine.transitions)
    {
        transition.sync = true;
    }
    MachineRuntime runtime;
    runtime.setValue(0U, 1.0F);
    advance(machine, runtime, 0.0F);
    REQUIRE(runtime.current == 1U);

    /* Half way through the one-second walk is the phase a two-second idle
       continues from. */
    runtime.time = 0.5F;
    runtime.setValue(0U, 0.0F);
    const Transition* taken = advance(machine, runtime, 0.0F);
    REQUIRE(taken != nullptr);
    REQUIRE(runtime.current == 0U);
    CHECK(std::fabs(runtime.time - 1.0F) < 0.0001F);
    CHECK(std::fabs(runtime.previous_time - 0.5F) < 0.0001F);
}
