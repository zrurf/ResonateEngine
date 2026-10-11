#ifndef RESONATE_RENDER_VULKAN_CAPABILITY_H
#define RESONATE_RENDER_VULKAN_CAPABILITY_H

#include "context.h"

#include <resonate/render/canvas.h>
#include <resonate/render/device.h>

namespace resonate::render_vulkan
{

/* Private module state, allocated from the host. Kept trivial because the host
   frees it without running constructors. */
struct State
{
    /* Module-owned; deleted in onDetach before the host frees this block. The
       backend's own objects are its own allocations, not host-tracked storage. */
    Context* context = nullptr;

    /* Held because creating a surface needs the canvas's native target, and the
       canvas is the only thing that knows whether there is one. */
    ResonateRenderCanvas* canvas = nullptr;
};

/* The one device this module publishes. Private to the library. */
ResonateRenderDevice& device();

} // namespace resonate::render_vulkan

#endif /* RESONATE_RENDER_VULKAN_CAPABILITY_H */
