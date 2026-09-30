#ifndef RESONATE_WINDOW_HOST_HPP
#define RESONATE_WINDOW_HOST_HPP

#include <resonate/render/canvas.h>
#include <resonate/window/input.h>
#include <resonate/window/window.h>

namespace resonate::window
{

/*
 * The capability structs this package publishes. Host-side: the startup layer
 * creates the window once and registers all three.
 */
ResonateWindow& windowCapability();
ResonateInput& inputCapability();

/* The window as a canvas: same state, and the same native-handle call the window
   capability makes, so a renderer that takes the canvas does exactly the work it
   would have done asking the window directly. */
ResonateRenderCanvas& windowCanvas();

} // namespace resonate::window

#endif /* RESONATE_WINDOW_HOST_HPP */
