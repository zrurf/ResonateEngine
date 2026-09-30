#include "capability.h"

#include <resonate/module/module.hpp>
#include <resonate/render/canvas.h>
#include <resonate/render/device.h>
#include <resonate/window/input.h>

namespace
{

using resonate::ui::State;

class UiModule final : public resonate::Module
{
  public:
    resonate::Status onAttach(resonate::Host& host) override
    {
        canvas_ = host.query<ResonateRenderCanvas>();
        host_ = host.raw();
        if (!canvas_)
        {
            return RESONATE_E_MISSING;
        }

        /* Both optional: a UI with no renderer and no input is a valid
           configuration, and neither is worth failing the attach over. */
        render_ = host.query<ResonateRenderDevice>();
        input_ = host.query<ResonateInput>();

        state_ = host.allocate<State>();
        if (state_ == nullptr)
        {
            return RESONATE_E_INTERNAL;
        }
        *state_ = State{};
        state_->display_size.x = static_cast<float>(canvas_->width(canvas_->self));
        state_->display_size.y = static_cast<float>(canvas_->height(canvas_->self));

        ResonateUiContext& published = resonate::ui::context();
        published.self = state_;
        return host.publish(published);
    }

    void onDetach() override
    {
        ResonateUiContext& published = resonate::ui::context();
        resonate::Host host(host_);
        host.unpublish(published);
        published.self = nullptr;
        host.deallocate(state_);
        input_ = {};
        render_ = {};
        canvas_ = {};
        state_ = nullptr;
    }

  private:
    resonate::CapabilityRef<ResonateRenderCanvas> canvas_;
    resonate::CapabilityRef<ResonateRenderDevice> render_;
    resonate::CapabilityRef<ResonateInput> input_;
    State* state_ = nullptr;

    /* Kept because onDetach receives no host. */
    const ResonateHostApi* host_ = nullptr;
};

} // namespace

RESONATE_DEFINE_MODULE(UiModule, "resonate.ui", "UI", "0.1.0")
