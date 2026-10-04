#include <catch2/catch_all.hpp>

#include <cstdint>
#include <limits>

#include <resonate/application/frame_clock.h>

/*
 * The frame time model: wall time in, whole fixed steps out, an interpolation
 * fraction left over, and a step budget that keeps a hitch from being paid back
 * as a burst.
 */

namespace
{

using resonate::FrameClock;

/* A 100Hz step: values that are exact halves and doubles of it keep the
   expected step counts free of floating-point ambiguity. */
FrameClock::Settings hundredHertz()
{
    FrameClock::Settings settings;
    settings.step_seconds = 0.01F;
    return settings;
}

} // namespace

TEST_CASE("a fixed step turns wall time into whole simulation steps", "[runtime][time]")
{
    FrameClock clock(hundredHertz());

    /* 5ms does not fill a 10ms step; the fraction stays in the accumulator. */
    REQUIRE(clock.advance(0.005F) == 0U);
    REQUIRE(clock.alpha() == Catch::Approx(0.5F));
    REQUIRE(clock.advance(0.015F) == 2U);
    REQUIRE(clock.alpha() == Catch::Approx(0.0F));

    /* A frame of exactly one step is one step, with nothing left over. */
    REQUIRE(clock.advance(0.01F) == 1U);
    REQUIRE(clock.alpha() == Catch::Approx(0.0F));
    REQUIRE(clock.steps() == 1U);
    REQUIRE(clock.stepSeconds() == Catch::Approx(0.01F));

    /* The leftovers add up: four 0.0025s frames make one step. */
    REQUIRE(clock.advance(0.0025F) == 0U);
    REQUIRE(clock.advance(0.0025F) == 0U);
    REQUIRE(clock.advance(0.0025F) == 0U);
    REQUIRE(clock.advance(0.0025F) == 1U);
    REQUIRE(clock.alpha() == Catch::Approx(0.0F));

    REQUIRE(clock.droppedSteps() == 0U);
}

TEST_CASE("a frame over the step budget is clamped and its debt dropped", "[runtime][time]")
{
    FrameClock clock(hundredHertz());

    /* Half a second owes fifty 10ms steps; the default budget runs five. */
    REQUIRE(clock.advance(0.5F) == FrameClock::kDefaultMaxSteps);
    REQUIRE(clock.droppedSteps() == 45U);

    /* The debt is gone rather than deferred: the next normal frame runs the
       one step it owes, not the remaining forty-five. */
    REQUIRE(clock.advance(0.01F) == 1U);
    REQUIRE(clock.droppedSteps() == 45U);

    /* A second stall drops again, on top of the first. */
    REQUIRE(clock.advance(0.2F) == FrameClock::kDefaultMaxSteps);
    REQUIRE(clock.droppedSteps() == 45U + 15U);
}

TEST_CASE("the step and the budget can be configured", "[runtime][time]")
{
    FrameClock::Settings settings;
    settings.step_seconds = 1.0F / 30.0F;
    settings.max_steps = 2U;
    FrameClock clock(settings);

    REQUIRE(clock.stepSeconds() == Catch::Approx(1.0F / 30.0F));

    /* One 30Hz step of wall time is one step. */
    REQUIRE(clock.advance(1.0F / 30.0F) == 1U);

    /* A second of wall time owes thirty of them; the budget runs two. */
    REQUIRE(clock.advance(1.0F) == 2U);
    REQUIRE(clock.droppedSteps() > 0U);

    /* Settings left at their defaults select the defaults. */
    FrameClock defaults((FrameClock::Settings{}));
    REQUIRE(defaults.stepSeconds() == Catch::Approx(FrameClock::kDefaultStepSeconds));
    REQUIRE(defaults.advance(1.0F) == FrameClock::kDefaultMaxSteps);
}

TEST_CASE("timescale and pause are time-flow parameters", "[runtime][time]")
{
    FrameClock clock(hundredHertz());

    /* Two frames of half a step each make one whole step of simulation. */
    clock.setTimeScale(0.5F);
    REQUIRE(clock.timeScale() == Catch::Approx(0.5F));
    REQUIRE(clock.advance(0.01F) == 0U);
    REQUIRE(clock.advance(0.01F) == 1U);

    /* A paused clock accumulates nothing, and drops nothing either: the wall
       time of the pause is not owed afterwards. The clock picks up where it
       stopped — still at half speed, since the scale is untouched by pause. */
    clock.setPaused(true);
    REQUIRE(clock.paused());
    REQUIRE(clock.advance(1.0F) == 0U);
    REQUIRE(clock.droppedSteps() == 0U);
    clock.setPaused(false);
    REQUIRE(clock.advance(0.01F) == 0U);
    REQUIRE(clock.advance(0.01F) == 1U);

    /* A zero scale freezes time without pausing. */
    clock.setTimeScale(0.0F);
    REQUIRE(clock.advance(1.0F) == 0U);
    REQUIRE(clock.droppedSteps() == 0U);

    /* A scale that is not a usable number leaves the current one. */
    clock.setTimeScale(-1.0F);
    REQUIRE(clock.timeScale() == Catch::Approx(0.0F));
    clock.setTimeScale(std::numeric_limits<float>::quiet_NaN());
    REQUIRE(clock.timeScale() == Catch::Approx(0.0F));
}

TEST_CASE("a frame time that is not positive advances nothing", "[runtime][time]")
{
    FrameClock clock(hundredHertz());

    REQUIRE(clock.advance(0.0F) == 0U);
    REQUIRE(clock.advance(-1.0F) == 0U);
    REQUIRE(clock.advance(std::numeric_limits<float>::quiet_NaN()) == 0U);
    REQUIRE(clock.advance(std::numeric_limits<float>::infinity()) == 0U);
    REQUIRE(clock.droppedSteps() == 0U);

    /* The clock is not poisoned by the bad values: a real frame still steps. */
    REQUIRE(clock.advance(0.01F) == 1U);
}

TEST_CASE("a frame time beyond any arithmetic leaves the clock sane", "[runtime][time]")
{
    FrameClock clock(hundredHertz());

    /* So much time that dropping it would be absorbed by the accumulator's own
       rounding: the clock still ends below one step, so the frame after it
       behaves normally instead of being permanently saturated. */
    REQUIRE(clock.advance(1.0e30F) == FrameClock::kDefaultMaxSteps);
    REQUIRE(clock.alpha() == Catch::Approx(0.0F));
    REQUIRE(clock.advance(0.01F) == 1U);
    REQUIRE(clock.alpha() == Catch::Approx(0.0F));
}
