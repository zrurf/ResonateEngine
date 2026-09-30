#ifndef RESONATE_RENDER_DEVICE_H
#define RESONATE_RENDER_DEVICE_H

#include <resonate/module/capability.h>

#ifdef __cplusplus
#    include <resonate/module/capability_id.hpp>
#endif
#include <resonate/module/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateRenderDevice
{
    ResonateCapabilityHeader header;

    /* Attaches the native surface the device presents to; RESONATE_E_INVALID for
       a null surface. */
    ResonateStatus (*attach_surface)(void* self, const void* native_surface, uint32_t width,
                                     uint32_t height);

    /* Begins and ends one pass; commands recorded between them are submitted together. */
    ResonateStatus (*begin_pass)(void* self, ResonateVec4 clear_color);
    ResonateStatus (*end_pass)(void* self);

    ResonateStatus (*set_viewport)(void* self, ResonateRect viewport);

    /* geometry is a provider-side handle; the caller never owns a pipeline object. */
    ResonateStatus (*submit_draw)(void* self, uint64_t geometry, ResonateMat4 view_projection,
                                  uint32_t vertex_count, uint32_t index_count);

    /* Returns RESONATE_E_STATE when no surface is attached. */
    ResonateStatus (*present)(void* self);

    /* Provider state; the host never interprets it. */
    void* self;
} ResonateRenderDevice;

#ifdef __cplusplus
} // extern "C"

namespace resonate::detail
{
template <> struct CapabilityTraits<ResonateRenderDevice>
{
    static constexpr const char* name = "Resonate.Render.Device";
    /* Inline so the whole program shares one definition; not constexpr, because
       the hash is a library call. */
    static inline const Id id{name};
    static constexpr std::uint32_t version = 1;
};
} // namespace resonate::detail
#endif

#endif /* RESONATE_RENDER_DEVICE_H */
