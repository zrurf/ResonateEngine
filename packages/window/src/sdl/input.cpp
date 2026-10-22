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

SDL_GamepadButton mapButton(ResonateGamepadButton button)
{
    switch (button)
    {
        case RESONATE_GAMEPAD_SOUTH:
            return SDL_GAMEPAD_BUTTON_SOUTH;
        case RESONATE_GAMEPAD_EAST:
            return SDL_GAMEPAD_BUTTON_EAST;
        case RESONATE_GAMEPAD_WEST:
            return SDL_GAMEPAD_BUTTON_WEST;
        case RESONATE_GAMEPAD_NORTH:
            return SDL_GAMEPAD_BUTTON_NORTH;
        case RESONATE_GAMEPAD_BACK:
            return SDL_GAMEPAD_BUTTON_BACK;
        case RESONATE_GAMEPAD_GUIDE:
            return SDL_GAMEPAD_BUTTON_GUIDE;
        case RESONATE_GAMEPAD_START:
            return SDL_GAMEPAD_BUTTON_START;
        case RESONATE_GAMEPAD_LEFT_STICK:
            return SDL_GAMEPAD_BUTTON_LEFT_STICK;
        case RESONATE_GAMEPAD_RIGHT_STICK:
            return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
        case RESONATE_GAMEPAD_LEFT_SHOULDER:
            return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
        case RESONATE_GAMEPAD_RIGHT_SHOULDER:
            return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
        case RESONATE_GAMEPAD_DPAD_UP:
            return SDL_GAMEPAD_BUTTON_DPAD_UP;
        case RESONATE_GAMEPAD_DPAD_DOWN:
            return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
        case RESONATE_GAMEPAD_DPAD_LEFT:
            return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
        case RESONATE_GAMEPAD_DPAD_RIGHT:
            return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
        default:
            return SDL_GAMEPAD_BUTTON_INVALID;
    }
}

SDL_GamepadAxis mapAxis(ResonateGamepadAxis axis)
{
    switch (axis)
    {
        case RESONATE_GAMEPAD_AXIS_LEFT_X:
            return SDL_GAMEPAD_AXIS_LEFTX;
        case RESONATE_GAMEPAD_AXIS_LEFT_Y:
            return SDL_GAMEPAD_AXIS_LEFTY;
        case RESONATE_GAMEPAD_AXIS_RIGHT_X:
            return SDL_GAMEPAD_AXIS_RIGHTX;
        case RESONATE_GAMEPAD_AXIS_RIGHT_Y:
            return SDL_GAMEPAD_AXIS_RIGHTY;
        case RESONATE_GAMEPAD_AXIS_LEFT_TRIGGER:
            return SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
        case RESONATE_GAMEPAD_AXIS_RIGHT_TRIGGER:
            return SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        default:
            return SDL_GAMEPAD_AXIS_INVALID;
    }
}

int32_t gamepadCount(void* self)
{
    const auto* state = static_cast<const WindowState*>(self);
    return state != nullptr ? state->gamepad_count : 0;
}

int32_t gamepadButtonDown(void* self, int32_t pad, ResonateGamepadButton button)
{
    const auto* state = static_cast<const WindowState*>(self);
    if (state == nullptr || pad < 0 || pad >= state->gamepad_count ||
        button >= RESONATE_GAMEPAD_BUTTON_COUNT)
    {
        return 0;
    }
    return SDL_GetGamepadButton(state->gamepads[pad], mapButton(button)) ? 1 : 0;
}

float gamepadAxis(void* self, int32_t pad, ResonateGamepadAxis axis)
{
    const auto* state = static_cast<const WindowState*>(self);
    if (state == nullptr || pad < 0 || pad >= state->gamepad_count ||
        axis >= RESONATE_GAMEPAD_AXIS_COUNT)
    {
        return 0.0F;
    }
    const Sint16 raw = SDL_GetGamepadAxis(state->gamepads[pad], mapAxis(axis));
    if (axis <= RESONATE_GAMEPAD_AXIS_RIGHT_Y)
    {
        const float value = static_cast<float>(raw) / 32767.0F;
        return value < -1.0F ? -1.0F : value;
    }
    return raw <= 0 ? 0.0F : static_cast<float>(raw) / 32767.0F;
}

ResonateInput g_input = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateInput, RESONATE_THREAD_MAIN),
    &keyDown,
    &keyPressed,
    &mouseDown,
    &pointerPosition,
    &pointerDelta,
    &gamepadCount,
    &gamepadButtonDown,
    &gamepadAxis,
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
