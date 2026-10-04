#ifndef RESONATE_APPLICATION_FRAME_CLOCK_H
#define RESONATE_APPLICATION_FRAME_CLOCK_H

#include <cstdint>

namespace resonate
{

/*
 * The frame's time model: wall-clock elapsed time in, fixed simulation steps
 * out, plus the fraction of a step already elapsed — the factor whatever draws
 * the frame interpolates the previous and current simulation states by.
 *
 * A frame advances the simulation by as many whole steps as its wall time
 * covers, and what is left over rides in the accumulator, so a jittery cadence
 * still steps the simulation at exactly the fixed rate. A frame whose time
 * covers more than the step budget runs the budget and drops the rest, which is
 * what keeps a hitch — a breakpoint, a suspended process, a machine stall —
 * from being paid back later as an avalanche of steps. What was dropped is
 * reported, so the run can say so.
 *
 * Timescale and pause are time-flow parameters: the scale multiplies wall time
 * before it is accumulated, and a paused clock accumulates nothing. Neither
 * stops the frame loop — rendering and whatever else the caller wired into the
 * frame keep running.
 */
class FrameClock
{
  public:
    struct Settings
    {
        /* Duration of one simulation step in seconds; not positive selects the
           default. */
        float step_seconds = 0.0F;

        /* Most steps one frame may run; zero selects the default. */
        std::uint32_t max_steps = 0U;

        /* Wall time multiplier; not finite or negative selects one. */
        float time_scale = 1.0F;

        bool paused = false;
    };

    static constexpr float kDefaultStepSeconds = 1.0F / 60.0F;

    /* Five 60Hz steps, ~83ms: a frame that covers more than that is already
       below 12fps, and chasing the debt would only make the next frame worse. */
    static constexpr std::uint32_t kDefaultMaxSteps = 5U;

    FrameClock();
    explicit FrameClock(const Settings& settings);

    /* Takes one frame's elapsed wall time in seconds and returns the fixed
       steps to simulate now. Non-positive and non-finite elapsed times advance
       nothing. */
    std::uint32_t advance(float elapsed_seconds);

    /* The delta to run each of the returned steps with. */
    [[nodiscard]] float stepSeconds() const noexcept;

    /* Steps the last advance() returned. */
    [[nodiscard]] std::uint32_t steps() const noexcept;

    /* How far into the next step the frame already is, in [0, 1). */
    [[nodiscard]] float alpha() const noexcept;

    /* Steps the last advance() dropped against the budget, accumulated over the
       clock's life. Zero while every frame fit. */
    [[nodiscard]] std::uint64_t droppedSteps() const noexcept;

    /* A scale that is not finite or is negative leaves the current one. */
    void setTimeScale(float scale);
    [[nodiscard]] float timeScale() const noexcept;

    void setPaused(bool paused);
    [[nodiscard]] bool paused() const noexcept;

  private:
    double step_seconds_;
    std::uint32_t max_steps_;
    double time_scale_ = 1.0;
    double accumulator_ = 0.0;
    std::uint32_t steps_ = 0;
    std::uint64_t dropped_steps_ = 0;
    bool paused_ = false;
};

} // namespace resonate

#endif /* RESONATE_APPLICATION_FRAME_CLOCK_H */
