#include "capability.h"

namespace resonate::ui
{
namespace
{

void begin(void* self, ResonateVec2 display_size, float scale)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr)
    {
        return;
    }
    state->display_size = display_size;
    state->scale = scale;
    state->in_pass = true;
}

void end(void* self)
{
    if (auto* state = static_cast<State*>(self))
    {
        state->in_pass = false;
    }
}

ResonateStatus button(void* self, const char* label, ResonateRect, int32_t* out_pressed)
{
    const auto* state = static_cast<const State*>(self);
    if (state == nullptr || label == nullptr || out_pressed == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    if (!state->in_pass)
    {
        return RESONATE_E_STATE;
    }
    *out_pressed = 0;
    return RESONATE_OK;
}

ResonateStatus text(void* self, const char* label, ResonateRect)
{
    const auto* state = static_cast<const State*>(self);
    if (state == nullptr || label == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    return state->in_pass ? RESONATE_OK : RESONATE_E_STATE;
}

ResonateStatus pointerPosition(void* self, ResonateVec2* out_position)
{
    const auto* state = static_cast<const State*>(self);
    if (state == nullptr || out_position == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    *out_position = state->pointer;
    return RESONATE_OK;
}

ResonateUiContext g_context = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateUiContext, RESONATE_THREAD_MAIN),
    &begin,
    &end,
    &button,
    &text,
    &pointerPosition,
    nullptr,
};

} // namespace

ResonateUiContext& context()
{
    return g_context;
}

} // namespace resonate::ui
