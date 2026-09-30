#include <catch2/catch_all.hpp>

#include <SDL3/SDL.h>

#include <resonate/window/host.hpp>

namespace
{

/* SDL is driven through its dummy video driver so the capability can be
   exercised without a display. */
struct DummyVideo
{
    DummyVideo()
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "dummy", 1);
    }
};

} // namespace

TEST_CASE("the window capability refuses a second window and reports its size", "[window]")
{
    const DummyVideo dummy;
    constexpr int width = 320;
    constexpr int height = 200;

    ResonateWindow& window = resonate::window::windowCapability();
    REQUIRE(window.self != nullptr);

    /* Required rather than skipped: the dummy driver is what makes this test
       runnable here at all, and a case that steps over its own body reports
       verification it did not do. */
    REQUIRE(window.create(window.self, "Resonate test", width, height) == RESONATE_OK);

    REQUIRE(window.create(window.self, "second", width, height) == RESONATE_E_STATE);
    REQUIRE(window.width(window.self) > 0U);
    REQUIRE(window.height(window.self) > 0U);

    window.pump_events(window.self);

    const ResonateInput& input = resonate::window::inputCapability();
    REQUIRE(input.self != nullptr);

    /* SDL exposes no way to set the keyboard state (only to reset it), so the
       snapshot can only be driven by real input. What is checked here is the
       wiring: the queries answer, and nothing is held on a frame with no input. */
    REQUIRE(input.key_down(input.self, RESONATE_KEY_SPACE) == 0);
    REQUIRE(input.key_pressed(input.self, RESONATE_KEY_SPACE) == 0);
    REQUIRE(input.mouse_down(input.self, RESONATE_MOUSE_LEFT) == 0);

    ResonateVec2 position = {};
    input.pointer_position(input.self, &position);
    REQUIRE(position.x >= 0.0F);
    REQUIRE(position.y >= 0.0F);

    /* Pre-filled with a value no window reports: the query has to write for this
       to pass, which a check against its own zero-initialised value would not
       catch. */
    ResonateVec2 written = {-12345.0F, -12345.0F};
    input.pointer_position(input.self, &written);
    REQUIRE(written.x != -12345.0F);
    REQUIRE(written.y != -12345.0F);

    ResonateVec2 delta = {-12345.0F, -12345.0F};
    input.pointer_delta(input.self, &delta);
    REQUIRE(delta.x != -12345.0F);
    REQUIRE(delta.y != -12345.0F);

    /* The canvas is the window in window mode: the same size, the same native
       target, and the same call underneath. That identity is what keeps a renderer
       that takes the canvas on the path it would have taken asking the window. */
    ResonateRenderCanvas& canvas = resonate::window::windowCanvas();
    REQUIRE(canvas.self != nullptr);
    REQUIRE(canvas.is_window_backed(canvas.self) == 1U);
    REQUIRE(canvas.width(canvas.self) == window.width(window.self));
    REQUIRE(canvas.height(canvas.self) == window.height(window.self));
    REQUIRE(canvas.native_handle(canvas.self) == window.native_handle(window.self));

    window.destroy(window.self);
    REQUIRE(window.width(window.self) == 0U);

    /* And with no window there is nothing to draw on, which is what a renderer
       checks for. */
    REQUIRE(canvas.width(canvas.self) == 0U);
    REQUIRE(canvas.native_handle(canvas.self) == nullptr);
}

TEST_CASE("a close request ends the window, which is how a caller reads a quit", "[window]")
{
    const DummyVideo dummy;

    ResonateWindow& window = resonate::window::windowCapability();
    REQUIRE(window.self != nullptr);
    REQUIRE(window.create(window.self, "Resonate test", 320, 200) == RESONATE_OK);
    REQUIRE(window.width(window.self) > 0U);

    /* What the platform queues when the user closes the window. The capability
       answers for it: the pump destroys the window, and width() is how a caller
       asks whether there is still one. */
    SDL_Event quit = {};
    quit.type = SDL_EVENT_QUIT;
    REQUIRE(SDL_PushEvent(&quit) == true);

    window.pump_events(window.self);

    REQUIRE(window.width(window.self) == 0U);
    REQUIRE(window.height(window.self) == 0U);
    REQUIRE(window.native_handle(window.self) == nullptr);

    /* And the pump of a window that is already gone is not a way back in. */
    window.pump_events(window.self);
    REQUIRE(window.width(window.self) == 0U);
}

TEST_CASE("the window capability refuses a size it cannot pass on", "[window]")
{
    const DummyVideo dummy;

    ResonateWindow& window = resonate::window::windowCapability();
    REQUIRE(window.self != nullptr);

    /* SDL takes the size as an int, so a larger value has to be refused here
       rather than wrapped into a negative one; a window with no size is not a
       window either. */
    REQUIRE(window.create(window.self, "too wide", 0xFFFFFFFFU, 200U) == RESONATE_E_INVALID);
    REQUIRE(window.create(window.self, "zero", 0U, 200U) == RESONATE_E_INVALID);
    REQUIRE(window.create(window.self, nullptr, 320U, 200U) == RESONATE_E_INVALID);
    REQUIRE(window.width(window.self) == 0U);
}
