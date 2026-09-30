#include "capability.h"

#include <cstdlib>
#include <string>

#include <resonate/module/module.hpp>

namespace
{

using resonate::audio::State;

uint32_t toSampleCount(const std::string& text, uint32_t fallback)
{
    return text.empty() ? fallback : static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
}

class AudioModule final : public resonate::Module
{
  public:
    resonate::Status onAttach(resonate::Host& host) override
    {
        host_ = host.raw();

        state_ = host.allocate<State>();
        if (state_ == nullptr)
        {
            return RESONATE_E_INTERNAL;
        }
        *state_ = State{};
        ResonateAudioDevice& published = resonate::audio::device();
        published.self = state_;

        const uint32_t sample_rate =
            toSampleCount(host.config("audio", "sample_rate", "48000"), 48000);
        const uint32_t channels = toSampleCount(host.config("audio", "channels", "2"), 2);
        published.open(state_, sample_rate, channels);
        return host.publish(published);
    }

    void onDetach() override
    {
        ResonateAudioDevice& published = resonate::audio::device();
        published.close(state_);
        resonate::Host host(host_);
        host.unpublish(published);
        published.self = nullptr;
        host.deallocate(state_);
        state_ = nullptr;
    }

  private:
    State* state_ = nullptr;

    /* Kept because onDetach receives no host. */
    const ResonateHostApi* host_ = nullptr;
};

} // namespace

RESONATE_DEFINE_MODULE(AudioModule, "resonate.audio", "Audio", "0.1.0")
