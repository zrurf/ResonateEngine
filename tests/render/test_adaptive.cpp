#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;

#include <resonate/renderer/adaptive.h>

namespace
{

/* Frame time follows the pixels: doubling the scale squares the cost, which is
   the model the controller's square-root law assumes. */
double frameTimeAt(float scale, double base_ms)
{
    return base_ms * static_cast<double>(scale) * static_cast<double>(scale);
}

} // namespace

TEST_CASE("adaptive scale: a slow window lowers the scale toward the budget", "[render][adaptive]")
{
    /* 2x over a 16.7 ms budget at full resolution: the law wants sqrt(0.5) of
       the pixels, but the step cap keeps one window's change bounded. */
    const float wanted = resonate::renderer::adaptiveScaleStep(1.0F, 33.4, 16.7F);
    CHECK(wanted == Approx(0.85F));
}

TEST_CASE("adaptive scale: a fast window raises the scale, bounded by the cap", "[render][adaptive]")
{
    const float wanted = resonate::renderer::adaptiveScaleStep(0.5F, 8.35, 16.7F);
    /* sqrt(2) of the current, capped at +0.1. */
    CHECK(wanted == Approx(0.6F));
}

TEST_CASE("adaptive scale: the deadband absorbs noise", "[render][adaptive]")
{
    CHECK(resonate::renderer::adaptiveScaleStep(0.7F, 16.7, 16.7F) == 0.7F);
    CHECK(resonate::renderer::adaptiveScaleStep(0.7F, 17.0, 16.7F) == 0.7F);
    CHECK(resonate::renderer::adaptiveScaleStep(0.7F, 14.5, 16.7F) == 0.7F);
}

TEST_CASE("adaptive scale: the step clamps at the configured bounds", "[render][adaptive]")
{
    /* Already at the floor and still slow: it cannot go lower. */
    CHECK(resonate::renderer::adaptiveScaleStep(resonate::renderer::kAdaptiveMinScale, 100.0,
                                                16.7F) == resonate::renderer::kAdaptiveMinScale);
    /* Already at full scale and fast: it cannot go higher. */
    CHECK(resonate::renderer::adaptiveScaleStep(1.0F, 4.0, 16.7F) == 1.0F);
    CHECK(resonate::renderer::adaptiveScaleStep(0.4F, 40.0, 16.7F, 0.5F) == 0.5F);
}

TEST_CASE("adaptive scale: rounds converge to the budget without oscillating", "[render][adaptive]")
{
    for (const double budget : {16.7, 33.3})
    {
        INFO("budget " << budget);
        /* The device needs 2x the budget at full scale. */
        const double base = budget * 2.0;
        float scale = 1.0F;
        int direction = 0;
        int direction_flips = 0;
        for (int window = 0; window < 12; ++window)
        {
            const float next =
                resonate::renderer::adaptiveScaleStep(scale, frameTimeAt(scale, base),
                                                      static_cast<float>(budget));
            const int step_direction = next > scale ? 1 : (next < scale ? -1 : 0);
            if (step_direction != 0)
            {
                if (direction != 0 && step_direction != direction)
                {
                    ++direction_flips;
                }
                direction = step_direction;
            }
            scale = next;
        }
        /* Converged: the settled scale's frame time is inside the deadband. */
        const double settled = frameTimeAt(scale, base);
        CHECK(settled <= budget * 1.15);
        CHECK(settled >= budget * 0.8);
        /* Never approached from both sides: a clipped proportional law with a
           deadband has no overshoot, so a flip would be a regression. */
        CHECK(direction_flips <= 1);
    }
}

TEST_CASE("adaptive scale: a zero budget keeps the scale alone", "[render][adaptive]")
{
    CHECK(resonate::renderer::adaptiveScaleStep(0.6F, 40.0, 0.0F) == 0.6F);
    CHECK(resonate::renderer::adaptiveScaleStep(0.6F, 0.0, 16.7F) == 0.6F);
}
