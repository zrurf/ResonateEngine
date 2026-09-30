#ifndef RESONATE_WINDOW_WINDOW_H
#define RESONATE_WINDOW_WINDOW_H

#include <resonate/module/capability.h>

#ifdef __cplusplus
#    include <resonate/module/capability_id.hpp>
#endif
#include <resonate/module/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Window and input source. One window per process; the runtime creates it
   before any module attaches. */
typedef struct ResonateWindow
{
    ResonateCapabilityHeader header;

    ResonateStatus (*create)(void* self, const char* title, uint32_t width, uint32_t height);
    void (*destroy)(void* self);

    /* Pumps the OS event queue. Must run once per frame before anything reads
       input. A quit request destroys the window here, so width() is also how a
       caller asks whether there is still a window. */
    void (*pump_events)(void* self);

    /* Back-buffer size in pixels, which is not the window coordinate space the
       input capability reports: the two differ wherever the display is scaled.
       Zero once the window is gone. */
    uint32_t (*width)(void* self);
    uint32_t (*height)(void* self);

    /* Native handle for a render backend; valid only while the window exists. */
    const void* (*native_handle)(void* self);

    /* Provider state. Every call above receives this as its first argument; the
       host never interprets it. */
    void* self;
} ResonateWindow;

#ifdef __cplusplus
} // extern "C"

namespace resonate::detail
{
template <> struct CapabilityTraits<ResonateWindow>
{
    static constexpr const char* name = "Resonate.Window";
    /* Inline so the whole program shares one definition; not constexpr, because
       the hash is a library call. */
    static inline const Id id{name};
    static constexpr std::uint32_t version = 1;
};
} // namespace resonate::detail
#endif

#endif /* RESONATE_WINDOW_WINDOW_H */
