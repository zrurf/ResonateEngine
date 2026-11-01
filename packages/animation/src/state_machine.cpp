#include <resonate/animation/state_machine.h>

#include <algorithm>
#include <cmath>

namespace resonate::animation
{
namespace
{

bool inRange(std::uint32_t parameter)
{
    return parameter < kMaxMachineParameters;
}

} // namespace

std::uint32_t StateMachine::parameterIndex(std::string_view name) const
{
    for (std::size_t index = 0U; index < parameters.size(); ++index)
    {
        if (parameters[index] == name)
        {
            return static_cast<std::uint32_t>(index);
        }
    }
    return kUnknownParameter;
}

bool StateMachine::validate(std::string& error) const
{
    if (states.empty())
    {
        error = "a state machine needs at least one state";
        return false;
    }
    if (initial >= states.size())
    {
        error = "the initial state is out of range";
        return false;
    }
    for (std::size_t index = 0U; index < transitions.size(); ++index)
    {
        const Transition& transition = transitions[index];
        if (transition.to >= states.size())
        {
            error = "transition " + std::to_string(index) + " targets an unknown state";
            return false;
        }
        if (transition.from != kAnyState && transition.from >= states.size())
        {
            error = "transition " + std::to_string(index) + " starts at an unknown state";
            return false;
        }
        for (const Condition& condition : transition.conditions)
        {
            if (!inRange(condition.parameter) || condition.parameter >= parameters.size())
            {
                error = "transition " + std::to_string(index) + " names an unknown parameter";
                return false;
            }
        }
    }
    if (parameters.size() > kMaxMachineParameters)
    {
        error = "more parameters than a runtime holds";
        return false;
    }
    return true;
}

void MachineRuntime::setValue(std::uint32_t parameter, float value)
{
    if (inRange(parameter))
    {
        values[parameter] = value;
    }
}

void MachineRuntime::setTrigger(std::uint32_t parameter)
{
    if (inRange(parameter))
    {
        triggers |= 1U << parameter;
    }
}

void MachineRuntime::clearTriggers()
{
    triggers = 0U;
}

float MachineRuntime::value(std::uint32_t parameter) const
{
    return inRange(parameter) ? values[parameter] : 0.0F;
}

bool MachineRuntime::triggered(std::uint32_t parameter) const
{
    return inRange(parameter) && (triggers & (1U << parameter)) != 0U;
}

float blendWeight(const MachineRuntime& runtime)
{
    if (runtime.fade_duration <= 0.0F)
    {
        return 1.0F;
    }
    return std::clamp(1.0F - runtime.fade_remaining / runtime.fade_duration, 0.0F, 1.0F);
}

bool conditionHolds(const Condition& condition, const MachineRuntime& runtime)
{
    const float value = runtime.value(condition.parameter);
    switch (condition.test)
    {
    case ConditionTest::Greater:
        return value > condition.value;
    case ConditionTest::Less:
        return value < condition.value;
    case ConditionTest::Equal:
        return std::fabs(value - condition.value) <= 1.0e-6F;
    case ConditionTest::NotEqual:
        return std::fabs(value - condition.value) > 1.0e-6F;
    case ConditionTest::True:
        return value != 0.0F;
    case ConditionTest::False:
        return value == 0.0F;
    case ConditionTest::Triggered:
        return runtime.triggered(condition.parameter);
    }
    return false;
}

const Transition* advance(const StateMachine& machine, MachineRuntime& runtime,
                          float delta_seconds)
{
    if (machine.states.empty())
    {
        return nullptr;
    }
    if (!runtime.started)
    {
        runtime.started = true;
        runtime.current = machine.initial < machine.states.size() ? machine.initial : 0U;
        runtime.previous = runtime.current;
        runtime.time = 0.0F;
        runtime.raw_time = 0.0F;
        runtime.previous_time = 0.0F;
        runtime.fade_duration = 0.0F;
        runtime.fade_remaining = 0.0F;
    }
    if (runtime.current >= machine.states.size())
    {
        return nullptr;
    }

    const State& state = machine.states[runtime.current];
    runtime.time += delta_seconds;
    runtime.raw_time += delta_seconds;
    if (state.looping && state.duration > 0.0F)
    {
        runtime.time = std::fmod(runtime.time, state.duration);
        if (runtime.time < 0.0F)
        {
            runtime.time += state.duration;
        }
    }
    const bool at_end = !state.looping && state.duration > 0.0F && runtime.time >= state.duration;

    if (runtime.fade_remaining > 0.0F)
    {
        runtime.fade_remaining = std::max(runtime.fade_remaining - delta_seconds, 0.0F);
        if (runtime.fade_remaining <= 0.0F)
        {
            runtime.previous = runtime.current;
            runtime.previous_time = runtime.time;
        }
    }

    const Transition* chosen = nullptr;
    for (const Transition& transition : machine.transitions)
    {
        if (transition.from != kAnyState && transition.from != runtime.current)
        {
            continue;
        }
        if (transition.at_end_only && !at_end)
        {
            continue;
        }
        bool holds = true;
        for (const Condition& condition : transition.conditions)
        {
            if (!conditionHolds(condition, runtime))
            {
                holds = false;
                break;
            }
        }
        if (!holds)
        {
            continue;
        }
        /* Lowest priority wins; on a tie the earlier declaration wins. */
        if (chosen == nullptr || transition.priority < chosen->priority)
        {
            chosen = &transition;
        }
    }
    if (chosen == nullptr)
    {
        return nullptr;
    }

    runtime.previous = runtime.current;
    runtime.previous_time = runtime.time;
    runtime.current = chosen->to;
    const State& target = machine.states[runtime.current];
    if (chosen->sync && state.duration > 0.0F && target.duration > 0.0F)
    {
        runtime.time = runtime.previous_time / state.duration * target.duration;
    }
    else
    {
        runtime.time = 0.0F;
    }
    /* A transition restarts the root-motion clock with the state: the entity
       keeps its place rather than jumping by the skipped part of the clip. */
    runtime.raw_time = 0.0F;
    runtime.fade_duration = std::max(chosen->fade_seconds, 0.0F);
    runtime.fade_remaining = runtime.fade_duration;
    return chosen;
}

} // namespace resonate::animation
