#include "capability.h"

namespace resonate::render
{
namespace
{

ResonateStatus attachSurface(void* self, const void* native_surface, uint32_t width,
                             uint32_t height)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || native_surface == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    state->surface_attached = true;
    state->width = width;
    state->height = height;
    return RESONATE_OK;
}

ResonateStatus beginPass(void*, ResonateVec4)
{
    return RESONATE_OK;
}

ResonateStatus endPass(void*)
{
    return RESONATE_OK;
}

ResonateStatus setViewport(void*, ResonateRect)
{
    return RESONATE_OK;
}

ResonateStatus submitDraw(void*, uint64_t geometry, ResonateMat4, uint32_t, uint32_t)
{
    return geometry != 0 ? RESONATE_OK : RESONATE_E_INVALID;
}

ResonateStatus present(void* self)
{
    const auto* state = static_cast<const State*>(self);
    return (state != nullptr && state->surface_attached) ? RESONATE_OK : RESONATE_E_STATE;
}

ResonateRenderDevice g_device = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateRenderDevice, RESONATE_THREAD_MAIN),
    &attachSurface,
    &beginPass,
    &endPass,
    &setViewport,
    &submitDraw,
    &present,
    nullptr,
};

} // namespace

ResonateRenderDevice& device()
{
    return g_device;
}

} // namespace resonate::render
