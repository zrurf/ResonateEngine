#ifndef RESONATE_UI_CAPABILITY_H
#define RESONATE_UI_CAPABILITY_H

#include <resonate/ui/context.h>

namespace resonate::ui
{

/* Private module state, allocated from the host. */
struct State
{
    ResonateVec2 display_size{0.0F, 0.0F};
    ResonateVec2 pointer{0.0F, 0.0F};
    float scale = 1.0F;
    bool in_pass = false;
};

/* The one context this module publishes. Private to the library. */
ResonateUiContext& context();

} // namespace resonate::ui

#endif /* RESONATE_UI_CAPABILITY_H */
