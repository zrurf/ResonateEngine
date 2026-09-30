#ifndef RESONATE_RENDER_CANVAS_H
#define RESONATE_RENDER_CANVAS_H

/*
 * Where a renderer draws.
 *
 * A window is one canvas, not the only one. An offscreen canvas has the same
 * shape and no native target, so a renderer that takes a canvas runs in a windowed
 * host, in a dedicated server or in a test without knowing which it is.
 *
 * Everything here is read, never written: a renderer resolves the capability in
 * on_attach and holds it, asking for the size when it may have changed.
 */

#include <resonate/module/capability.h>

#ifdef __cplusplus
#    include <resonate/module/capability_id.hpp>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateRenderCanvas
{
    ResonateCapabilityHeader header;

    /* Back-buffer size in pixels. Zero once there is nothing to draw on. */
    uint32_t (*width)(void* self);
    uint32_t (*height)(void* self);

    /* 1 when the target belongs to the platform — a window's surface, which a
       backend presents through its own swapchain — and 0 when it is offscreen and
       the renderer owns the target. */
    uint32_t (*is_window_backed)(void* self);

    /* Native target for a backend: an HWND, a CAMetalLayer, an X11 window. NULL
       when the canvas is not window-backed. Valid only while there is a size. */
    const void* (*native_handle)(void* self);

    /* Provider state; the host never interprets it. */
    void* self;
} ResonateRenderCanvas;

#ifdef __cplusplus
} // extern "C"

namespace resonate::detail
{
template <> struct CapabilityTraits<ResonateRenderCanvas>
{
    static constexpr const char* name = "Resonate.Render.Canvas";
    /* Inline so the whole program shares one definition; not constexpr, because
       the hash is a library call. */
    static inline const Id id{name};
    static constexpr std::uint32_t version = 1;
};
} // namespace resonate::detail
#endif

#endif /* RESONATE_RENDER_CANVAS_H */
