#include <resonate/audio/device.h>

#include <resonate/module/module_descriptor.hpp>

RESONATE_MODULE_DESCRIPTOR("resonate.audio", "Audio", "0.1.0", 1,
                           RESONATE_CAPABILITIES(ResonateAudioDevice), RESONATE_CAPABILITIES(),
                           RESONATE_CAPABILITIES(), RESONATE_MODULE_NAMES())
