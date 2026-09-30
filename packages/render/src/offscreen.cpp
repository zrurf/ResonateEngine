#include <resonate/render/offscreen.hpp>

#include <resonate/render/canvas.h>

namespace resonate::render
{
namespace
{

struct OffscreenState
{
    uint32_t width = 0;
    uint32_t height = 0;
};

OffscreenState g_state;

uint32_t width(void* self)
{
    return static_cast<const OffscreenState*>(self)->width;
}

uint32_t height(void* self)
{
    return static_cast<const OffscreenState*>(self)->height;
}

uint32_t isWindowBacked(void*)
{
    return 0;
}

const void* nativeHandle(void*)
{
    return nullptr;
}

ResonateRenderCanvas g_canvas = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateRenderCanvas, RESONATE_THREAD_MAIN),
    &width,
    &height,
    &isWindowBacked,
    &nativeHandle,
    nullptr,
};

} // namespace

ResonateStatus publishOffscreen(ResonateCapabilityRegistry* registry, uint32_t width_value,
                                uint32_t height_value)
{
    if (registry == nullptr || width_value == 0U || height_value == 0U)
    {
        return RESONATE_E_INVALID;
    }

    g_state.width = width_value;
    g_state.height = height_value;
    g_canvas.self = &g_state;

    ResonateCapabilityRecord record = {};
    record.id = detail::CapabilityTraits<ResonateRenderCanvas>::id.value();
    record.version = detail::CapabilityTraits<ResonateRenderCanvas>::version;
    record.instance = &g_canvas;
    record.struct_size = g_canvas.header.struct_size;
    record.name = detail::CapabilityTraits<ResonateRenderCanvas>::name;
    return resonate_capability_register(registry, &record);
}

} // namespace resonate::render
