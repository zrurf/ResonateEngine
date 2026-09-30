#include <resonate/window/input.h>

#include <SDL3/SDL.h>

#include <resonate/window/host.hpp>

#include "internal.h"

namespace resonate::window
{
namespace
{

int32_t keyDown(void* self, ResonateKey key)
{
    const auto* state = static_cast<const WindowState*>(self);
    if (state == nullptr || key >= RESONATE_KEY_COUNT)
    {
        return 0;
    }
    return state->input.held[key] ? 1 : 0;
}

int32_t keyPressed(void* self, ResonateKey key)
{
    const auto* state = static_cast<const WindowState*>(self);
    if (state == nullptr || key >= RESONATE_KEY_COUNT)
    {
        return 0;
    }
    return state->input.pressed[key] ? 1 : 0;
}

int32_t mouseDown(void* self, ResonateMouseButton button)
{
    const auto* state = static_cast<const WindowState*>(self);
    if (state == nullptr || button >= RESONATE_MOUSE_BUTTON_COUNT)
    {
        return 0;
    }
    return state->input.mouse[button] ? 1 : 0;
}

void pointerPosition(void* self, ResonateVec2* out_position)
{
    const auto* state = static_cast<const WindowState*>(self);
    if (state == nullptr || out_position == nullptr)
    {
        return;
    }
    out_position->x = state->input.pointer[0];
    out_position->y = state->input.pointer[1];
}

void pointerDelta(void* self, ResonateVec2* out_delta)
{
    const auto* state = static_cast<const WindowState*>(self);
    if (state == nullptr || out_delta == nullptr)
    {
        return;
    }
    out_delta->x = state->input.delta[0];
    out_delta->y = state->input.delta[1];
}

ResonateInput g_input = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateInput, RESONATE_THREAD_MAIN),
    &keyDown,
    &keyPressed,
    &mouseDown,
    &pointerPosition,
    &pointerDelta,
    nullptr,
};

} // namespace

bool mouseButtonMask(ResonateMouseButton button, uint32_t& mask)
{
    switch (button)
    {
        case RESONATE_MOUSE_LEFT:
            mask = SDL_BUTTON_LMASK;
            return true;
        case RESONATE_MOUSE_RIGHT:
            mask = SDL_BUTTON_RMASK;
            return true;
        case RESONATE_MOUSE_MIDDLE:
            mask = SDL_BUTTON_MMASK;
            return true;
        case RESONATE_MOUSE_X1:
            mask = SDL_BUTTON_X1MASK;
            return true;
        case RESONATE_MOUSE_X2:
            mask = SDL_BUTTON_X2MASK;
            return true;
        default:
            return false;
    }
}

ResonateInput& inputCapability()
{
    if (g_input.self == nullptr)
    {
        g_input.self = &windowState();
    }
    return g_input;
}

} // namespace resonate::window
