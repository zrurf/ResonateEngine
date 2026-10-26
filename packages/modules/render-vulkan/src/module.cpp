#include "capability.h"

#include <resonate/module/module.hpp>
#include <resonate/render/canvas.h>

namespace
{

using resonate::render_vulkan::State;

class RenderVulkanModule final : public resonate::Module
{
  public:
    resonate::Status onAttach(resonate::Host& host) override
    {
        /* A canvas rather than a window: the same device draws into a window
           and into an offscreen target, and this module cannot tell which it
           got. No canvas at all — a server run — skips the backend cleanly. */
        canvas_ = host.query<ResonateRenderCanvas>();
        host_ = host.raw();
        if (!canvas_)
        {
            return RESONATE_E_UNSUPPORTED;
        }

        /* One device per run: the configuration names its backend, and a second
           one to attach is a skip, not a collision. Both checks answer before
           anything is taken. */
        const std::optional<std::string> selected = host.readConfig("resonate.render", "backend");
        if (selected.has_value() && *selected != "vulkan")
        {
            return RESONATE_E_UNSUPPORTED;
        }

        state_ = host.allocate<State>();
        if (state_ == nullptr)
        {
            return RESONATE_E_INTERNAL;
        }
        *state_ = State{};
        state_->canvas = &*canvas_;
        /* Vsync and the color space come from the run's configuration, read
           once here; the swapchain they decide is created on the first
           window. "linear" hands the transfer function to the final pass. */
        const std::optional<std::string> color_space =
            host.readConfig("resonate.render", "color-space");
        const bool prefer_srgb = color_space != std::optional<std::string>("linear");
        state_->context = new resonate::render_vulkan::Context(
            host.readBool("resonate.render", "vsync", true), prefer_srgb, &*canvas_);

        ResonateRenderDevice& published = resonate::render_vulkan::device();
        published.self = state_;
        if (host.publish(published) != RESONATE_OK)
        {
            /* Another device provider is live: leave nothing behind and skip. */
            delete state_->context;
            state_->context = nullptr;
            host.deallocate(state_);
            canvas_ = {};
            state_ = nullptr;
            return RESONATE_E_UNSUPPORTED;
        }
        return RESONATE_OK;
    }

    void onDetach() override
    {
        resonate::Host host(host_);
        ResonateRenderDevice& published = resonate::render_vulkan::device();
        host.unpublish(published);
        published.self = nullptr;
        delete state_->context;
        state_->context = nullptr;
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

RESONATE_DEFINE_MODULE(RenderVulkanModule, "resonate.render-vulkan", "Render Vulkan", "0.1.0")
