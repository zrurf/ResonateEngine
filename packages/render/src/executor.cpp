#include <resonate/render/executor.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace resonate::render
{
namespace
{

/* RESONATE_FRAME_PROFILE: splits a frame into the device's frame boundary
   (acquire), the recorded passes, and the present, which is where a stuck
   frame on a device shows up. Single-threaded: only the render thread uses it. */
struct ExecutorProfile
{
    double begin_ms = 0.0;
    double passes_ms = 0.0;
    double present_ms = 0.0;
    unsigned frames = 0U;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};

ExecutorProfile g_profile;

/* Read on first use, not at static initialization: the platform sets the
   environment after the process starts (Android passes it as a property). */
bool profileEnabled()
{
    static const bool enabled = std::getenv("RESONATE_FRAME_PROFILE") != nullptr;
    return enabled;
}

/* RESONATE_RENDER_CLEAR_ONLY skips every pass; RESONATE_RENDER_SKIP_PASSES names
   passes (comma separated) to skip one at a time. Both leave the presented
   frame stale, so they exist only to attribute a frame's GPU cost. */
bool skipPasses()
{
    static const bool skip = std::getenv("RESONATE_RENDER_CLEAR_ONLY") != nullptr;
    return skip;
}

bool skipPassNamed(const char* name)
{
    static const std::string listing = [] {
        const char* value = std::getenv("RESONATE_RENDER_SKIP_PASSES");
        return std::string(value != nullptr ? value : "");
    }();
    if (name == nullptr || listing.empty())
    {
        return false;
    }
    const std::string target = name;
    std::size_t start = 0U;
    while (start < listing.size())
    {
        const std::size_t comma = listing.find(',', start);
        const std::string_view entry(listing.data() + start,
                                     (comma == std::string::npos ? listing.size() : comma) - start);
        if (entry == target)
        {
            return true;
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1U;
    }
    return false;
}

double millisSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

void reportProfile()
{
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_profile.since).count();
    if (elapsed < 1.0)
    {
        return;
    }
    const double frames = g_profile.frames > 0U ? static_cast<double>(g_profile.frames) : 1.0;
    std::fprintf(stderr, "render: profile begin %.2f ms, passes %.2f ms, present %.2f ms\n",
                 g_profile.begin_ms / frames, g_profile.passes_ms / frames,
                 g_profile.present_ms / frames);
    g_profile.begin_ms = 0.0;
    g_profile.passes_ms = 0.0;
    g_profile.present_ms = 0.0;
    g_profile.frames = 0U;
    g_profile.since = std::chrono::steady_clock::now();
}

} // namespace

GraphExecutor::GraphExecutor(ResonateRenderDevice& device, const GraphPlan& plan, ResourceId canvas,
                             bool coalesce_barriers)
    : device_(device), plan_(plan), canvas_(canvas), coalesce_barriers_(coalesce_barriers)
{
    batch_supported_ =
        device_.header.struct_size >=
            offsetof(ResonateRenderDevice, barrier_batch) + sizeof(device_.barrier_batch) &&
        device_.barrier_batch != nullptr;
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
            /* A depth sample is not filterable everywhere, and a shadow lookup
               never wants linear filtering: nearest is the portable choice. */
            desc.nearest =
                (plan_resource.usage & RESONATE_RENDER_IMAGE_DEPTH_ATTACHMENT) != 0U ? 1U : 0U;
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

ResonateRenderBarrierDesc GraphExecutor::describe(const Barrier& barrier) const
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
    return desc;
}

bool GraphExecutor::apply(const Barrier& barrier)
{
    const ResonateRenderBarrierDesc desc = describe(barrier);
    return device_.barrier(device_.self, &desc) == RESONATE_OK;
}

bool GraphExecutor::applyAll(const std::vector<Barrier>& barriers, const char* failure,
                             std::string& out_error)
{
    if (!coalesce_barriers_ || !batch_supported_)
    {
        for (const Barrier& barrier : barriers)
        {
            if (barrier.resource == canvas_)
            {
                continue; /* the device owns the canvas's layout flow */
            }
            if (!apply(barrier))
            {
                out_error = failure;
                return false;
            }
        }
        return true;
    }

    /* The compat tier: consecutive same-direction barriers share one device
       call. The canvas keeps its own path, and a direction flip flushes the
       run so a read-after-write never merges with the write it depends
       on. */
    constexpr std::uint32_t kWrites =
        RESONATE_RENDER_ACCESS_SHADER_WRITE | RESONATE_RENDER_ACCESS_COLOR_WRITE |
        RESONATE_RENDER_ACCESS_DEPTH_WRITE | RESONATE_RENDER_ACCESS_TRANSFER_WRITE;
    std::vector<ResonateRenderBarrierDesc> batch;
    batch.reserve(barriers.size());
    const auto flush = [&]() -> bool
    {
        if (batch.empty())
        {
            return true;
        }
        const bool ok =
            device_.barrier_batch(device_.self, batch.data(),
                                  static_cast<std::uint32_t>(batch.size())) == RESONATE_OK;
        batch.clear();
        return ok;
    };
    bool writing = false;
    bool have_run = false;
    for (const Barrier& barrier : barriers)
    {
        if (barrier.resource == canvas_)
        {
            continue;
        }
        const bool barrier_writes = (barrier.after_access & kWrites) != 0U;
        if (have_run && barrier_writes != writing)
        {
            if (!flush())
            {
                out_error = failure;
                return false;
            }
        }
        writing = barrier_writes;
        have_run = true;
        batch.push_back(describe(barrier));
    }
    if (!flush())
    {
        out_error = failure;
        return false;
    }
    return true;
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
    const auto begin_start = std::chrono::steady_clock::now();
    if (device_.begin_frame(device_.self) != RESONATE_OK)
    {
        return false;
    }
    if (profileEnabled())
    {
        g_profile.begin_ms += millisSince(begin_start);
    }
    const auto passes_start = std::chrono::steady_clock::now();

    GraphExecuteContext context{&device_, images_.data(),      buffers_.data(),
                                static_cast<std::uint32_t>(images_.size()),
                                static_cast<std::uint32_t>(buffers_.size())};
    if (prepare != nullptr)
    {
        prepare(prepare_context, context);
    }
    for (const PlanStep& step : plan_.steps)
    {
        if (!applyAll(step.barriers, "a barrier was refused", out_error))
        {
            device_.present(device_.self);
            return false;
        }
        const PlanPass& pass = plan_.passes[step.pass];
        if (pass.execute != nullptr && !skipPasses() && !skipPassNamed(pass.name))
        {
            pass.execute(pass.context, context);
        }
    }

    if (!applyAll(plan_.final_barriers, "a final barrier was refused", out_error))
    {
        device_.present(device_.self);
        return false;
    }

    if (profileEnabled())
    {
        g_profile.passes_ms += millisSince(passes_start);
    }
    const auto present_start = std::chrono::steady_clock::now();
    device_.present(device_.self);
    if (profileEnabled())
    {
        g_profile.present_ms += millisSince(present_start);
        ++g_profile.frames;
        reportProfile();
    }
    return true;
}

} // namespace resonate::render
