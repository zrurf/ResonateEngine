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

#include <cstddef>
#include <string>
#include <vector>

namespace resonate::render
{

struct GraphExecuteContext
{
    ResonateRenderDevice* device = nullptr;

    /* Resolved handles, indexed by resource id; the canvas import is {0}. An id
       outside the table — an invalid resource a pass named — reads as the null
       handle rather than walking past the vector. */
    const ResonateRenderImage* images = nullptr;
    const ResonateRenderBuffer* buffers = nullptr;
    std::uint32_t image_count = 0U;
    std::uint32_t buffer_count = 0U;

    [[nodiscard]] ResonateRenderImage image(ResourceId resource) const
    {
        return resource < image_count ? images[resource] : ResonateRenderImage{};
    }
    [[nodiscard]] ResonateRenderBuffer buffer(ResourceId resource) const
    {
        return resource < buffer_count ? buffers[resource] : ResonateRenderBuffer{};
    }
    /* The table slot the device assigned to the resource. The handle's low
       half is its pool index — vertex and index buffers have one without a
       table slot — so the device answers when it can. */
    [[nodiscard]] std::uint32_t imageSlot(ResourceId resource) const
    {
        const ResonateRenderImage handle = image(resource);
        if (handle.value == 0U)
        {
            return 0U;
        }
        if (device != nullptr &&
            device->header.struct_size >=
                offsetof(ResonateRenderDevice, image_slot) + sizeof(device->image_slot) &&
            device->image_slot != nullptr)
        {
            return device->image_slot(device->self, handle);
        }
        return static_cast<std::uint32_t>(handle.value & 0xFFFFFFFFU);
    }
    [[nodiscard]] std::uint32_t bufferSlot(ResourceId resource) const
    {
        const ResonateRenderBuffer handle = buffer(resource);
        if (handle.value == 0U)
        {
            return 0U;
        }
        if (device != nullptr &&
            device->header.struct_size >=
                offsetof(ResonateRenderDevice, buffer_slot) + sizeof(device->buffer_slot) &&
            device->buffer_slot != nullptr)
        {
            return device->buffer_slot(device->self, handle);
        }
        return static_cast<std::uint32_t>(handle.value & 0xFFFFFFFFU);
    }
};

class GraphExecutor
{
  public:
    /* The plan is borrowed and must outlive the executor; `canvas` names the
       imported resource the device presents through. `coalesce_barriers`
       batches each step's consecutive same-direction barriers into one device
       call when the device offers barrier_batch — the tile-based compat tier
       prefers it; a desktop tier keeps the fine-grained per-barrier calls
       (IGL/Daxa R2). */
    GraphExecutor(ResonateRenderDevice& device, const GraphPlan& plan, ResourceId canvas,
                  bool coalesce_barriers = false);
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
    [[nodiscard]] ResonateRenderBarrierDesc describe(const Barrier& barrier) const;
    bool apply(const Barrier& barrier);
    /* One step's (or the final list's) barriers in order: the per-barrier
       path, or consecutive same-direction runs through barrier_batch.
       `failure` labels the error. */
    bool applyAll(const std::vector<Barrier>& barriers, const char* failure,
                  std::string& out_error);

    ResonateRenderDevice& device_;
    const GraphPlan& plan_;
    ResourceId canvas_;
    bool coalesce_barriers_ = false;
    bool batch_supported_ = false;
    std::vector<ResonateRenderImage> images_;
    std::vector<ResonateRenderBuffer> buffers_;
    bool materialized_ = false;
    bool materialize_failed_ = false;
};

} // namespace resonate::render

#endif /* RESONATE_RENDER_EXECUTOR_H */
