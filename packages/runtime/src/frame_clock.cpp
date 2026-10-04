#include <resonate/application/frame_clock.h>

#include <cmath>
#include <limits>

namespace resonate
{
namespace
{

bool usableStep(float step_seconds)
{
    return std::isfinite(step_seconds) && step_seconds > 0.0F;
}

bool usableScale(float scale)
{
    return std::isfinite(scale) && scale >= 0.0F;
}

} // namespace

FrameClock::FrameClock() : FrameClock(Settings{})
{
}

FrameClock::FrameClock(const Settings& settings)
    : step_seconds_(usableStep(settings.step_seconds) ? static_cast<double>(settings.step_seconds)
                                                      : static_cast<double>(kDefaultStepSeconds)),
      max_steps_(settings.max_steps != 0U ? settings.max_steps : kDefaultMaxSteps),
      time_scale_(usableScale(settings.time_scale) ? static_cast<double>(settings.time_scale)
                                                   : 1.0),
      paused_(settings.paused)
{
}

std::uint32_t FrameClock::advance(float elapsed_seconds)
{
    steps_ = 0;

    const double elapsed = static_cast<double>(elapsed_seconds);
    if (!paused_ && std::isfinite(elapsed) && elapsed > 0.0)
    {
        accumulator_ += elapsed * time_scale_;

        const double requested_raw = accumulator_ / step_seconds_;
        std::uint32_t requested =
            requested_raw >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())
                ? std::numeric_limits<std::uint32_t>::max()
                : static_cast<std::uint32_t>(requested_raw);

        if (requested > max_steps_)
        {
            /* The frame owes more than the budget: run the budget and drop the
               rest, so the debt dies here instead of compounding. */
            const std::uint32_t dropped = requested - max_steps_;
            accumulator_ -= static_cast<double>(dropped) * step_seconds_;
            dropped_steps_ += dropped;
            requested = max_steps_;
        }

        steps_ = requested;
        accumulator_ -= static_cast<double>(steps_) * step_seconds_;
        if (!(accumulator_ < step_seconds_))
        {
            /* The remainder has to end up below one step, and the subtraction
               is what puts it there. Rounding can miss by an ulp, and a frame
               time so large that the subtracted time is absorbed entirely
               would leave the accumulator where it was — either way nothing
               useful is left to keep, so it starts over from zero. */
            accumulator_ = 0.0;
        }
    }

    return steps_;
}

float FrameClock::stepSeconds() const noexcept
{
    return static_cast<float>(step_seconds_);
}

std::uint32_t FrameClock::steps() const noexcept
{
    return steps_;
}

float FrameClock::alpha() const noexcept
{
    return static_cast<float>(accumulator_ / step_seconds_);
}

std::uint64_t FrameClock::droppedSteps() const noexcept
{
    return dropped_steps_;
}

void FrameClock::setTimeScale(float scale)
{
    if (usableScale(scale))
    {
        time_scale_ = static_cast<double>(scale);
    }
}

float FrameClock::timeScale() const noexcept
{
    return static_cast<float>(time_scale_);
}

void FrameClock::setPaused(bool paused)
{
    paused_ = paused;
}

bool FrameClock::paused() const noexcept
{
    return paused_;
}

} // namespace resonate
