#include <resonate/window/window.h>

#include <climits>

#include <SDL3/SDL.h>

#include <resonate/window/host.hpp>

#include "internal.h"

namespace resonate::window
{
namespace
{

WindowState g_state;

/* Defined below; create opens the pads that are already connected, and the
   event pump follows hotplug. */
bool openGamepad(WindowState& state, SDL_JoystickID id);
void closeGamepad(WindowState& state, SDL_JoystickID id);

ResonateStatus create(void* self, const char* title, uint32_t width, uint32_t height)
{
    auto* state = static_cast<WindowState*>(self);
    /* The size reaches SDL as an int, so anything past that range has to be
       refused here rather than wrapped into a negative one. */
    constexpr uint32_t MAX_DIMENSION = static_cast<uint32_t>(INT_MAX);
    if (state == nullptr || title == nullptr || width == 0U || height == 0U ||
        width > MAX_DIMENSION || height > MAX_DIMENSION)
    {
        return RESONATE_E_INVALID;
    }
    if (state->handle != nullptr)
    {
        return RESONATE_E_STATE;
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        return RESONATE_E_UNSUPPORTED;
    }

    const SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    state->handle =
        SDL_CreateWindow(title, static_cast<int>(width), static_cast<int>(height), flags);
    if (state->handle == nullptr)
    {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RESONATE_E_INTERNAL;
    }
    SDL_StartTextInput(state->handle);

    /* Gamepads are optional: a machine without controller support still runs
       with keyboard and mouse. Pads already connected when the window opens are
       opened here; later ones arrive as events. */
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD))
    {
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count))
        {
            for (int index = 0; index < count; ++index)
            {
                openGamepad(*state, ids[index]);
            }
            SDL_free(ids);
        }
    }

    int pixels_w = 0;
    int pixels_h = 0;
    SDL_GetWindowSizeInPixels(state->handle, &pixels_w, &pixels_h);
    state->width = static_cast<uint32_t>(pixels_w);
    state->height = static_cast<uint32_t>(pixels_h);
    return RESONATE_OK;
}

void destroy(void* self)
{
    auto* state = static_cast<WindowState*>(self);
    if (state == nullptr || state->handle == nullptr)
    {
        return;
    }

    for (int index = 0; index < state->gamepad_count; ++index)
    {
        SDL_CloseGamepad(state->gamepads[index]);
        state->gamepads[index] = nullptr;
    }
    state->gamepad_count = 0;
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);

    SDL_DestroyWindow(state->handle);
    state->handle = nullptr;
    state->width = 0;
    state->height = 0;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool openGamepad(WindowState& state, SDL_JoystickID id)
{
    for (int index = 0; index < state.gamepad_count; ++index)
    {
        if (state.gamepad_ids[index] == id)
        {
            return false;
        }
    }
    if (state.gamepad_count >= WindowState::kMaxGamepads)
    {
        return false;
    }
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (pad == nullptr)
    {
        return false;
    }
    state.gamepads[state.gamepad_count] = pad;
    state.gamepad_ids[state.gamepad_count] = id;
    ++state.gamepad_count;
    return true;
}

void closeGamepad(WindowState& state, SDL_JoystickID id)
{
    for (int index = 0; index < state.gamepad_count; ++index)
    {
        if (state.gamepad_ids[index] != id)
        {
            continue;
        }
        SDL_CloseGamepad(state.gamepads[index]);
        /* The last pad fills the hole; the engine's pad indices are positional,
           so a disconnect shifts only the tail. */
        state.gamepads[index] = state.gamepads[state.gamepad_count - 1];
        state.gamepad_ids[index] = state.gamepad_ids[state.gamepad_count - 1];
        state.gamepads[state.gamepad_count - 1] = nullptr;
        --state.gamepad_count;
        return;
    }
}

void refreshInput(InputState& input)
{
    const bool* keyboard = SDL_GetKeyboardState(nullptr);
    for (int key = 0; key < RESONATE_KEY_COUNT; ++key)
    {
        const bool down = keyboard != nullptr && keyboard[key];
        input.pressed[key] = down && !input.held[key];
        input.held[key] = down;
    }

    float x = 0.0F;
    float y = 0.0F;
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
    input.pointer[0] = x;
    input.pointer[1] = y;

    for (int button = 0; button < RESONATE_MOUSE_BUTTON_COUNT; ++button)
    {
        uint32_t mask = 0;
        input.mouse[button] = mouseButtonMask(static_cast<ResonateMouseButton>(button), mask) &&
                              (buttons & mask) != 0U;
    }

    /* SDL_GetRelativeMouseState resets the relative state, so this must run once
       per frame. */
    float dx = 0.0F;
    float dy = 0.0F;
    SDL_GetRelativeMouseState(&dx, &dy);
    input.delta[0] = dx;
    input.delta[1] = dy;
}

void pumpEvents(void* self)
{
    auto* state = static_cast<WindowState*>(self);
    if (state == nullptr || state->handle == nullptr)
    {
        return;
    }

    SDL_Event event = {};
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                destroy(state);
                return;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
            {
                int pixels_w = 0;
                int pixels_h = 0;
                SDL_GetWindowSizeInPixels(state->handle, &pixels_w, &pixels_h);
                state->width = static_cast<uint32_t>(pixels_w);
                state->height = static_cast<uint32_t>(pixels_h);
                break;
            }
            case SDL_EVENT_TEXT_INPUT:
                if (event.text.text != nullptr)
                {
                    state->input.text.append(event.text.text);
                }
                /* A commit ends the preedit, whether or not the IME follows it
                   with an empty editing event. */
                state->input.editing.clear();
                state->input.editing_cursor = -1;
                break;
            case SDL_EVENT_TEXT_EDITING:
                state->input.editing = event.edit.text != nullptr ? event.edit.text : "";
                state->input.editing_cursor =
                    state->input.editing.empty() ? -1 : event.edit.start;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                openGamepad(*state, event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                closeGamepad(*state, event.gdevice.which);
                break;
            default:
                break;
        }
    }

    refreshInput(state->input);
}

uint32_t queryWidth(void* self)
{
    const auto* state = static_cast<const WindowState*>(self);
    return state != nullptr ? state->width : 0U;
}

uint32_t queryHeight(void* self)
{
    const auto* state = static_cast<const WindowState*>(self);
    return state != nullptr ? state->height : 0U;
}

/* What a render backend needs to create its own surface; null on a platform
   whose native handle is not wired up. */
const void* nativeHandle(void* self)
{
    const auto* state = static_cast<const WindowState*>(self);
    if (state == nullptr || state->handle == nullptr)
    {
        return nullptr;
    }

    const SDL_PropertiesID properties = SDL_GetWindowProperties(state->handle);
#if defined(RESONATE_PLATFORM_WINDOWS)
    return SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(RESONATE_PLATFORM_MACOS) || defined(RESONATE_PLATFORM_IOS)
    return SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#else
    return nullptr;
#endif
}

ResonateWindow g_window = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateWindow, RESONATE_THREAD_MAIN),
    &create,
    &destroy,
    &pumpEvents,
    &queryWidth,
    &queryHeight,
    &nativeHandle,
    nullptr,
};

/*
 * The window as a canvas. Same state block, and the accessors above are what the
 * canvas dispatches to, so a renderer that takes the canvas does exactly the work
 * it did asking the window directly: the canvas costs a lookup at attach, not a
 * hop per frame.
 */
uint32_t canvasIsWindowBacked(void*)
{
    return 1;
}

ResonateRenderCanvas g_canvas = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateRenderCanvas, RESONATE_THREAD_MAIN),
    &queryWidth,
    &queryHeight,
    &canvasIsWindowBacked,
    &nativeHandle,
    nullptr,
};

} // namespace

WindowState& windowState()
{
    return g_state;
}

ResonateWindow& windowCapability()
{
    if (g_window.self == nullptr)
    {
        g_window.self = &g_state;
    }
    return g_window;
}

ResonateRenderCanvas& windowCanvas()
{
    if (g_canvas.self == nullptr)
    {
        g_canvas.self = &g_state;
    }
    return g_canvas;
}

} // namespace resonate::window
