#include <resonate/render/canvas.h>
#include <resonate/render/device.h>
#include <resonate/ui/context.h>
#include <resonate/window/input.h>

#include <resonate/module/module_descriptor.hpp>

RESONATE_MODULE_DESCRIPTOR("resonate.ui", "UI", "0.1.0", 1,
                           RESONATE_CAPABILITIES(ResonateUiContext),
                           RESONATE_CAPABILITIES(ResonateRenderCanvas),
                           RESONATE_CAPABILITIES(ResonateInput, ResonateRenderDevice),
                           RESONATE_MODULE_NAMES("resonate.render"))
