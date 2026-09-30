#include "capability.h"

#include <resonate/module/module.hpp>
#include <resonate/render/canvas.h>

namespace
{

using resonate::render::State;

class RenderModule final : public resonate::Module
{
  public:
    resonate::Status onAttach(resonate::Host& host) override
    {
        /* A canvas rather than a window: the same code draws into a window and
           into an offscreen target, and this module cannot tell which it got. */
        canvas_ = host.query<ResonateRenderCanvas>();
        host_ = host.raw();
        if (!canvas_)
        {
            return RESONATE_E_MISSING;
        }

        state_ = host.allocate<State>();
        if (state_ == nullptr)
        {
            return RESONATE_E_INTERNAL;
        }
        *state_ = State{};
        state_->canvas = &*canvas_;
        state_->width = canvas_->width(canvas_->self);
        state_->height = canvas_->height(canvas_->self);

        ResonateRenderDevice& published = resonate::render::device();
        published.self = state_;
        return host.publish(published);
    }

    void onDetach() override
    {
        ResonateRenderDevice& published = resonate::render::device();
        resonate::Host host(host_);
        host.unpublish(published);
        published.self = nullptr;
        host.deallocate(state_);
        canvas_ = {};
        state_ = nullptr;
    }

  private:
    resonate::CapabilityRef<ResonateRenderCanvas> canvas_;
    State* state_ = nullptr;

    /* Kept because onDetach receives no host. */
    const ResonateHostApi* host_ = nullptr;
};

} // namespace

RESONATE_DEFINE_MODULE(RenderModule, "resonate.render", "Render", "0.1.0")
