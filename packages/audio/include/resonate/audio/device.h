#ifndef RESONATE_AUDIO_DEVICE_H
#define RESONATE_AUDIO_DEVICE_H

#include <resonate/module/capability.h>

#ifdef __cplusplus
#    include <resonate/module/capability_id.hpp>
#endif
#include <resonate/module/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Audio output.
 *
 * The device is opened during attach and closed during detach. A host with no
 * output device still gets a valid capability whose fallible calls return
 * RESONATE_E_UNAVAILABLE.
 */
typedef struct ResonateAudioDevice
{
    ResonateCapabilityHeader header;

    ResonateStatus (*open)(void* self, uint32_t sample_rate, uint32_t channels);

    /* Queues interleaved samples and copies them, so the caller may reuse the
       buffer on return. Returns RESONATE_E_UNAVAILABLE when not open. */
    ResonateStatus (*submit)(void* self, const void* interleaved_samples, uint32_t frame_count);

    /* Linear gain, 1.0 being unity. */
    ResonateStatus (*set_volume)(void* self, float volume);

    /* Total frames played: the master clock other systems synchronise to. */
    uint64_t (*played_frames)(void* self);

    void (*close)(void* self);

    /* Provider state; the host never interprets it. */
    void* self;
} ResonateAudioDevice;

#ifdef __cplusplus
} // extern "C"

namespace resonate::detail
{
template <> struct CapabilityTraits<ResonateAudioDevice>
{
    static constexpr const char* name = "Resonate.Audio.Device";
    /* Inline so the whole program shares one definition; not constexpr, because
       the hash is a library call. */
    static inline const Id id{name};
    static constexpr std::uint32_t version = 1;
};
} // namespace resonate::detail
#endif

#endif /* RESONATE_AUDIO_DEVICE_H */
