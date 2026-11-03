#ifndef RESONATE_RENDERER_ADAPTIVE_H
#define RESONATE_RENDERER_ADAPTIVE_H

/*
 * The dynamic-resolution controller: one decision per sample window, from the
 * window's average frame time and the budget the run asked to hold. Frame time
 * is roughly linear in pixels, so the square root of the budget ratio is the
 * scale that would hit the budget; the step is bounded and a deadband absorbs
 * noise, so a window cannot swing resolution and a steady run does not
 * oscillate around the budget.
 */

#include <algorithm>
#include <cmath>

namespace resonate::renderer
{

inline constexpr float kAdaptiveMinScale = 0.35F;

/* The scale the next window renders at, given the current one. Answers
   `current` unchanged when the average sits inside the deadband. */
inline float adaptiveScaleStep(float current, double average_ms, float budget_ms,
                               float min_scale = kAdaptiveMinScale) noexcept
{
    if (average_ms <= 0.0 || budget_ms <= 0.0F)
    {
        return current;
    }
    if (average_ms <= static_cast<double>(budget_ms) * 1.15 &&
        average_ms >= static_cast<double>(budget_ms) * 0.85)
    {
        return current;
    }
    const float ratio = budget_ms / static_cast<float>(average_ms);
    const float wanted = std::clamp(current * std::sqrt(ratio), current - 0.15F, current + 0.1F);
    return std::clamp(wanted, min_scale, 1.0F);
}

} // namespace resonate::renderer

#endif /* RESONATE_RENDERER_ADAPTIVE_H */
