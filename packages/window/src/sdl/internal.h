#ifndef RESONATE_WINDOW_SRC_INTERNAL_H
#define RESONATE_WINDOW_SRC_INTERNAL_H

#include <cstdint>

#include <SDL3/SDL.h>

#include <resonate/render/canvas.h>
#include <resonate/window/input.h>
#include <resonate/window/window.h>

namespace resonate::window
{

/* Window and input share one state block; the input snapshot is refreshed when
   the event queue is pumped. */
struct InputState
{
    bool held[RESONATE_KEY_COUNT] = {};
    bool pressed[RESONATE_KEY_COUNT] = {};
    float pointer[2] = {};
    float delta[2] = {};
    bool mouse[RESONATE_MOUSE_BUTTON_COUNT] = {};
};

struct WindowState
{
    /* Four pads is a couch-multiplayer bound, not a driver limit: the action map
       asks for the count and indexes below it. */
    static constexpr int kMaxGamepads = 4;

    SDL_Window* handle = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    InputState input;
    SDL_Gamepad* gamepads[kMaxGamepads] = {};
    SDL_JoystickID gamepad_ids[kMaxGamepads] = {};
    int gamepad_count = 0;
};

WindowState& windowState();

/* Translates this engine's mouse numbering to SDL's button mask. */
bool mouseButtonMask(ResonateMouseButton button, uint32_t& mask);

} // namespace resonate::window

#endif /* RESONATE_WINDOW_SRC_INTERNAL_H */
