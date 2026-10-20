#include <resonate/render/executor.h>

#include <cstdio>

namespace resonate::render
{

GraphExecutor::GraphExecutor(ResonateRenderDevice& device, const GraphPlan& plan, ResourceId canvas)
    : device_(device), plan_(plan), canvas_(canvas)
{
}

GraphExecutor::~GraphExecutor()
{
    if (!materialized_)
    {
        return;
    }
    for (ResourceId resource = 0; resource < plan_.resources.size(); ++resource)
    {
        if (plan_.resources[resource].imported)
        {
            continue;
        }
        if (images_[resource].value != 0U)
        {
            device_.destroy_image(device_.self, images_[resource]);
        }
        if (buffers_[resource].value != 0U)
        {
            device_.destroy_buffer(device_.self, buffers_[resource]);
        }
    }
}

bool GraphExecutor::materialize(std::string& out_error)
{
    images_.assign(plan_.resources.size(), ResonateRenderImage{});
    buffers_.assign(plan_.resources.size(), ResonateRenderBuffer{});
    /* From here on the destructor owns whatever exists, a failed creation
       included. */
    materialized_ = true;
    for (ResourceId resource = 0; resource < plan_.resources.size(); ++resource)
    {
        const PlanResource& plan_resource = plan_.resources[resource];
        if (plan_resource.imported)
        {
            if (resource != canvas_)
            {
                out_error = std::string("import '") +
                            (plan_resource.name != nullptr ? plan_resource.name : "?") +
                            "' has no backing; only the canvas import is supported";
                return false;
            }
            continue; /* {0}: the device's swapchain convention */
        }

        if (plan_resource.kind == ResourceKind::Image)
        {
            ResonateRenderImageDesc desc{};
            desc.struct_size = sizeof(desc);
            desc.format = plan_resource.format;
            desc.width = plan_resource.width;
            desc.height = plan_resource.height;
            desc.usage = plan_resource.usage;
            if (device_.create_image(device_.self, &desc, &images_[resource]) != RESONATE_OK)
            {
                out_error = std::string("image '") + plan_resource.name + "' was not created";
                return false;
            }
        }
        else
        {
            ResonateRenderBufferDesc desc{};
            desc.struct_size = sizeof(desc);
            desc.size_bytes = plan_resource.size_bytes;
            desc.usage = plan_resource.usage;
            desc.host_visible = 0U;
            if (device_.create_buffer(device_.self, &desc, &buffers_[resource]) != RESONATE_OK)
            {
                out_error = std::string("buffer '") + plan_resource.name + "' was not created";
                return false;
            }
        }
    }
    materialized_ = true;
    return true;
}

bool GraphExecutor::apply(const Barrier& barrier)
{
    ResonateRenderBarrierDesc desc{};
    desc.struct_size = sizeof(desc);
    if (plan_.resources[barrier.resource].kind == ResourceKind::Image)
    {
        desc.image = images_[barrier.resource];
    }
    else
    {
        desc.buffer = buffers_[barrier.resource];
    }
    desc.before_stages = barrier.before_stages;
    desc.after_stages = barrier.after_stages;
    desc.before_access = barrier.before_access;
    desc.after_access = barrier.after_access;
    desc.before_layout = barrier.before_layout;
    desc.after_layout = barrier.after_layout;
    return device_.barrier(device_.self, &desc) == RESONATE_OK;
}

bool GraphExecutor::execute(std::string& out_error, PrepareFn prepare, void* prepare_context)
{
    out_error.clear();
    if (materialize_failed_)
    {
        return false;
    }
    /* Materializing lands resources in the device's resource table, which the
       frame binds when it opens — so it happens before begin_frame. */
    if (!materialized_ && !materialize(out_error))
    {
        materialize_failed_ = true;
        return false;
    }
    if (device_.begin_frame(device_.self) != RESONATE_OK)
    {
        return false;
    }

    GraphExecuteContext context{&device_, images_.data(), buffers_.data()};
    if (prepare != nullptr)
    {
        prepare(prepare_context, context);
    }
    for (const PlanStep& step : plan_.steps)
    {
        for (const Barrier& barrier : step.barriers)
        {
            if (barrier.resource == canvas_)
            {
                continue; /* the device owns the canvas's layout flow */
            }
            if (!apply(barrier))
            {
                out_error = "a barrier was refused";
                device_.present(device_.self);
                return false;
            }
        }
        const PlanPass& pass = plan_.passes[step.pass];
        if (pass.execute != nullptr)
        {
            pass.execute(pass.context, context);
        }
    }

    for (const Barrier& barrier : plan_.final_barriers)
    {
        if (barrier.resource == canvas_)
        {
            continue; /* the device takes the canvas into present itself */
        }
        if (!apply(barrier))
        {
            out_error = "a final barrier was refused";
            device_.present(device_.self);
            return false;
        }
    }

    device_.present(device_.self);
    return true;
}

} // namespace resonate::render
