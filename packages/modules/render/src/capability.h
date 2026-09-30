#ifndef RESONATE_RENDER_CAPABILITY_H
#define RESONATE_RENDER_CAPABILITY_H

#include <resonate/render/canvas.h>
#include <resonate/render/device.h>

namespace resonate::render
{

/* Private module state, allocated from the host. */
struct State
{
    bool surface_attached = false;
    uint32_t width = 0;
    uint32_t height = 0;

    /* Held because creating a surface needs the canvas's native target, and the
       canvas is the only thing that knows whether there is one. */
    ResonateRenderCanvas* canvas = nullptr;
};

/* The one device this module publishes. Private to the library. */
ResonateRenderDevice& device();

} // namespace resonate::render

#endif /* RESONATE_RENDER_CAPABILITY_H */
