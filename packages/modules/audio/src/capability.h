#ifndef RESONATE_AUDIO_CAPABILITY_H
#define RESONATE_AUDIO_CAPABILITY_H

#include <resonate/audio/device.h>

namespace resonate::audio
{

/* Private module state, allocated from the host. */
struct State
{
    bool opened = false;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    float volume = 1.0F;
    uint64_t played_frames = 0;
};

/* The one device this module publishes. Private to the library. */
ResonateAudioDevice& device();

} // namespace resonate::audio

#endif /* RESONATE_AUDIO_CAPABILITY_H */
