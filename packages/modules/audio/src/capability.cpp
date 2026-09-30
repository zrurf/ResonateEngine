#include "capability.h"

namespace resonate::audio
{
namespace
{

ResonateStatus open(void* self, uint32_t sample_rate, uint32_t channels)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr)
    {
        return RESONATE_E_STATE;
    }
    state->sample_rate = sample_rate;
    state->channels = channels;
    state->opened = true;
    return RESONATE_OK;
}

ResonateStatus submit(void* self, const void* interleaved_samples, uint32_t frame_count)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || interleaved_samples == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    if (!state->opened)
    {
        return RESONATE_E_UNAVAILABLE;
    }
    state->played_frames += frame_count;
    return RESONATE_OK;
}

ResonateStatus setVolume(void* self, float volume)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr)
    {
        return RESONATE_E_STATE;
    }
    state->volume = volume;
    return RESONATE_OK;
}

uint64_t playedFrames(void* self)
{
    const auto* state = static_cast<const State*>(self);
    return state != nullptr ? state->played_frames : 0;
}

void close(void* self)
{
    if (auto* state = static_cast<State*>(self))
    {
        state->opened = false;
    }
}

ResonateAudioDevice g_device = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateAudioDevice, RESONATE_THREAD_MAIN),
    &open,
    &submit,
    &setVolume,
    &playedFrames,
    &close,
    nullptr,
};

} // namespace

ResonateAudioDevice& device()
{
    return g_device;
}

} // namespace resonate::audio
