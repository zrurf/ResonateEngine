#include <resonate/render/canvas.h>
#include <resonate/render/device.h>

#include <resonate/module/module_descriptor.hpp>

RESONATE_MODULE_DESCRIPTOR("resonate.render", "Render", "0.1.0", 1,
                           RESONATE_CAPABILITIES(ResonateRenderDevice),
                           RESONATE_CAPABILITIES(ResonateRenderCanvas), RESONATE_CAPABILITIES(),
                           RESONATE_MODULE_NAMES())
