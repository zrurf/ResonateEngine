#include <resonate/window/window.h>

#include <android/input.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <resonate/window/android.hpp>
#include <resonate/window/host.hpp>

namespace resonate::window
{
namespace
{

struct AndroidState
{
    android_app* app = nullptr;
    ANativeWindow* window = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool pressed[RESONATE_MOUSE_BUTTON_COUNT] = {};
    bool held[RESONATE_MOUSE_BUTTON_COUNT] = {};
    float pointer[2] = {};
    float delta[2] = {};
};

AndroidState g_state;

void resizeWindow()
{
    if (g_state.window == nullptr)
    {
        g_state.width = 0;
        g_state.height = 0;
        return;
    }
    g_state.width = static_cast<uint32_t>(ANativeWindow_getWidth(g_state.window));
    g_state.height = static_cast<uint32_t>(ANativeWindow_getHeight(g_state.window));
}

void adoptWindow(ANativeWindow* window)
{
    if (window == nullptr || g_state.window != nullptr)
    {
        return;
    }
    g_state.window = window;
    ANativeWindow_acquire(window);
    resizeWindow();
}

void onAppCmd(android_app* app, int32_t command)
{
    switch (command)
    {
        case APP_CMD_INIT_WINDOW:
            adoptWindow(app->window);
            break;
        case APP_CMD_WINDOW_RESIZED:
            resizeWindow();
            break;
        case APP_CMD_TERM_WINDOW:
            if (g_state.window != nullptr)
            {
                ANativeWindow_release(g_state.window);
                g_state.window = nullptr;
                resizeWindow();
            }
            break;
        default:
            break;
    }
}

/* The pointer is the first touch; there is one button until a game asks for
   more, and no keyboard mapping yet. */
int32_t onInput(android_app*, AInputEvent* event)
{
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
    {
        return 0;
    }

    const float x = AMotionEvent_getX(event, 0);
    const float y = AMotionEvent_getY(event, 0);
    g_state.delta[0] += x - g_state.pointer[0];
    g_state.delta[1] += y - g_state.pointer[1];
    g_state.pointer[0] = x;
    g_state.pointer[1] = y;

    switch (AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK)
    {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            g_state.pressed[RESONATE_MOUSE_LEFT] =
                g_state.pressed[RESONATE_MOUSE_LEFT] || !g_state.held[RESONATE_MOUSE_LEFT];
            g_state.held[RESONATE_MOUSE_LEFT] = true;
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            g_state.held[RESONATE_MOUSE_LEFT] = false;
            break;
        default:
            break;
    }
    return 1;
}

/* Drains the app's event queue. Zero waits for an event; a negative value
   polls, which is what the frame loop wants. */
void pollEvents(int timeout_ms)
{
    for (;;)
    {
        int events = 0;
        android_poll_source* source = nullptr;
        const int result = ALooper_pollOnce(timeout_ms, nullptr, &events,
                                            reinterpret_cast<void**>(&source));
        if (result == ALOOPER_POLL_TIMEOUT || result == ALOOPER_POLL_ERROR)
        {
            return;
        }
        if (source != nullptr)
        {
            source->process(g_state.app, source);
        }
        if (g_state.app != nullptr && g_state.app->destroyRequested != 0)
        {
            return;
        }
        timeout_ms = 0;
    }
}

ResonateStatus create(void*, const char* title, uint32_t width, uint32_t height)
{
    (void)title;
    (void)width;
    (void)height;

    if (g_state.app == nullptr || g_state.window != nullptr)
    {
        return RESONATE_E_STATE;
    }

    /* INIT_WINDOW arrives on the app's looper, sometimes after android_main
       starts; the device needs a target before it can attach, so wait for it. */
    constexpr int kWindowWaitMs = 5000;
    for (int waited = 0; g_state.window == nullptr && waited < kWindowWaitMs; waited += 100)
    {
        pollEvents(100);
        if (g_state.app->destroyRequested != 0)
        {
            break;
        }
    }
    return g_state.window != nullptr ? RESONATE_OK : RESONATE_E_UNSUPPORTED;
}

void destroy(void*)
{
    if (g_state.window != nullptr)
    {
        ANativeWindow_release(g_state.window);
        g_state.window = nullptr;
    }
    g_state.width = 0;
    g_state.height = 0;
}

void pumpEvents(void*)
{
    for (uint32_t button = 0; button < RESONATE_MOUSE_BUTTON_COUNT; ++button)
    {
        g_state.pressed[button] = false;
    }
    g_state.delta[0] = 0.0F;
    g_state.delta[1] = 0.0F;

    pollEvents(0);
}

uint32_t queryWidth(void*)
{
    return g_state.width;
}

uint32_t queryHeight(void*)
{
    return g_state.height;
}

const void* nativeHandle(void*)
{
    return g_state.window;
}

uint32_t canvasIsWindowBacked(void*)
{
    return 1;
}

int32_t keyDown(void*, ResonateKey)
{
    return 0;
}

int32_t keyPressed(void*, ResonateKey)
{
    return 0;
}

int32_t mouseDown(void*, ResonateMouseButton button)
{
    return button < RESONATE_MOUSE_BUTTON_COUNT && g_state.held[button] ? 1 : 0;
}

void pointerPosition(void*, ResonateVec2* out_position)
{
    if (out_position == nullptr)
    {
        return;
    }
    out_position->x = g_state.pointer[0];
    out_position->y = g_state.pointer[1];
}

void pointerDelta(void*, ResonateVec2* out_delta)
{
    if (out_delta == nullptr)
    {
        return;
    }
    out_delta->x = g_state.delta[0];
    out_delta->y = g_state.delta[1];
    g_state.delta[0] = 0.0F;
    g_state.delta[1] = 0.0F;
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

ResonateRenderCanvas g_canvas = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateRenderCanvas, RESONATE_THREAD_MAIN),
    &queryWidth,
    &queryHeight,
    &canvasIsWindowBacked,
    &nativeHandle,
    nullptr,
};

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

void attachAndroidApp(android_app* app)
{
    g_state.app = app;
    if (app == nullptr)
    {
        return;
    }

    app->onAppCmd = &onAppCmd;
    app->onInputEvent = &onInput;

    /* The activity may have created the window before android_main ran. */
    adoptWindow(app->window);
}

ResonateWindow& windowCapability()
{
    return g_window;
}

ResonateRenderCanvas& windowCanvas()
{
    return g_canvas;
}

ResonateInput& inputCapability()
{
    return g_input;
}

} // namespace resonate::window
