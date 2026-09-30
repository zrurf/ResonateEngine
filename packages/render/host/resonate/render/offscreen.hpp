#ifndef RESONATE_RENDER_OFFSCREEN_HPP
#define RESONATE_RENDER_OFFSCREEN_HPP

/*
 * An offscreen canvas for a host that has no window: the size is what the run was
 * asked to render at, and the target belongs to whoever draws on it.
 *
 * Host-side; a module resolves the capability, never this.
 */

#include <cstdint>

#include <resonate/module/capability.h>

namespace resonate::render
{

/* Publishes the one offscreen canvas this process has. Fails with
   RESONATE_E_INVALID for a size that is not positive. */
ResonateStatus publishOffscreen(ResonateCapabilityRegistry* registry, uint32_t width,
                                uint32_t height);

} // namespace resonate::render

#endif /* RESONATE_RENDER_OFFSCREEN_HPP */
