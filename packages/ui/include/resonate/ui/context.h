#ifndef RESONATE_UI_CONTEXT_H
#define RESONATE_UI_CONTEXT_H

#include <resonate/module/capability.h>

#ifdef __cplusplus
#    include <resonate/module/capability_id.hpp>
#endif
#include <resonate/module/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Immediate-mode UI. */
typedef struct ResonateUiContext
{
    ResonateCapabilityHeader header;

    /* Starts and ends one pass; button and text between them are recorded into a
       display list consumed during RESONATE_STAGE_RENDER. Outside a pass they
       return RESONATE_E_STATE. */
    void (*begin)(void* self, ResonateVec2 display_size, float scale);
    void (*end)(void* self);

    /* out_pressed is 1 for the frame in which the button was released inside its bounds. */
    ResonateStatus (*button)(void* self, const char* label, ResonateRect bounds,
                             int32_t* out_pressed);
    ResonateStatus (*text)(void* self, const char* label, ResonateRect bounds);

    /* Hit-tested pointer position for the current frame, in display space. */
    ResonateStatus (*pointer_position)(void* self, ResonateVec2* out_position);

    /* Provider state; the host never interprets it. */
    void* self;
} ResonateUiContext;

#ifdef __cplusplus
} // extern "C"

namespace resonate::detail
{
template <> struct CapabilityTraits<ResonateUiContext>
{
    static constexpr const char* name = "Resonate.UI.Context";
    /* Inline so the whole program shares one definition; not constexpr, because
       the hash is a library call. */
    static inline const Id id{name};
    static constexpr std::uint32_t version = 1;
};
} // namespace resonate::detail
#endif

#endif /* RESONATE_UI_CONTEXT_H */
