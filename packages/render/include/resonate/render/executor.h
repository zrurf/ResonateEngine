#ifndef RESONATE_RENDER_EXECUTOR_H
#define RESONATE_RENDER_EXECUTOR_H

/* Runs a compiled graph plan against an RHI device, one frame at a time.
 *
 * A frame is: open the frame, replay each step's derived barriers, invoke the
 * pass callbacks — which scope their own render passes and draws through the
 * context — apply the final barriers, and present. Transient resources are
 * materialized once and reused every frame; the graph's barriers treat each
 * frame as starting discarded, so a transient's first use in a frame must be a
 * write. The canvas import needs no backing: its handle reads as {0}, the
 * device's swapchain convention, and the device owns its layout flow — every
 * canvas barrier the plan derives is the device's, not the executor's.
 */

#include <resonate/render/graph.h>

#include <string>
#include <vector>

namespace resonate::render
{

struct GraphExecuteContext
{
    ResonateRenderDevice* device = nullptr;

    /* Resolved handles, indexed by resource id; the canvas import is {0}. */
    const ResonateRenderImage* images = nullptr;
    const ResonateRenderBuffer* buffers = nullptr;

    [[nodiscard]] ResonateRenderImage image(ResourceId resource) const
    {
        return images[resource];
    }
    [[nodiscard]] ResonateRenderBuffer buffer(ResourceId resource) const
    {
        return buffers[resource];
    }
    /* A handle's low half is its resource-table slot (device.h). */
    [[nodiscard]] std::uint32_t imageSlot(ResourceId resource) const
    {
        return static_cast<std::uint32_t>(images[resource].value & 0xFFFFFFFFU);
    }
    [[nodiscard]] std::uint32_t bufferSlot(ResourceId resource) const
    {
        return static_cast<std::uint32_t>(buffers[resource].value & 0xFFFFFFFFU);
    }
};

class GraphExecutor
{
  public:
    /* The plan is borrowed and must outlive the executor; `canvas` names the
       imported resource the device presents through. */
    GraphExecutor(ResonateRenderDevice& device, const GraphPlan& plan, ResourceId canvas);
    ~GraphExecutor();

    GraphExecutor(const GraphExecutor&) = delete;
    GraphExecutor& operator=(const GraphExecutor&) = delete;

    /* Runs one frame. False when the device could not open one — a headless
       run, most often — with nothing else attempted; a failure inside the
       frame names itself in `error`. `prepare`, when given, runs after the
       frame opens and before the plan's steps: per-frame resources are written
       there, once the device has waited on the frames in flight. */
    using PrepareFn = void (*)(void* context, GraphExecuteContext& commands);
    bool execute(std::string& out_error, PrepareFn prepare = nullptr,
                 void* prepare_context = nullptr);

  private:
    bool materialize(std::string& out_error);
    bool apply(const Barrier& barrier);

    ResonateRenderDevice& device_;
    const GraphPlan& plan_;
    ResourceId canvas_;
    std::vector<ResonateRenderImage> images_;
    std::vector<ResonateRenderBuffer> buffers_;
    bool materialized_ = false;
    bool materialize_failed_ = false;
};

} // namespace resonate::render

#endif /* RESONATE_RENDER_EXECUTOR_H */
