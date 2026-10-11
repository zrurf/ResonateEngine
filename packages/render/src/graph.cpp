#include <resonate/render/graph.h>

#include <algorithm>
#include <cstring>

namespace resonate::render
{
namespace
{

bool accessIsWrite(AccessFlags access)
{
    constexpr AccessFlags kWrites =
        kAccessShaderWrite | kAccessColorWrite | kAccessDepthWrite | kAccessTransferWrite;
    return (access & kWrites) != 0U;
}

/* The state one resource carries between passes: the last use's vocabulary. */
struct TrackedState
{
    StageFlags stages = kStageNone;
    AccessFlags access = kAccessNone;
    ImageLayout layout = ImageLayout::Undefined;
    bool ever_used = false;
    bool last_was_write = false;
};

} // namespace

bool RenderGraph::isWrite(AccessFlags access)
{
    return accessIsWrite(access);
}

ResourceId RenderGraph::declareImage(const char* name, ResonateRenderFormat format,
                                     std::uint32_t width, std::uint32_t height)
{
    DeclaredResource declared;
    declared.plan.name = name;
    declared.plan.kind = ResourceKind::Image;
    declared.plan.format = format;
    declared.plan.width = width;
    declared.plan.height = height;
    resources_.push_back(declared);
    return static_cast<ResourceId>(resources_.size() - 1U);
}

ResourceId RenderGraph::declareBuffer(const char* name, std::uint64_t size_bytes)
{
    DeclaredResource declared;
    declared.plan.name = name;
    declared.plan.kind = ResourceKind::Buffer;
    declared.plan.size_bytes = size_bytes;
    resources_.push_back(declared);
    return static_cast<ResourceId>(resources_.size() - 1U);
}

ResourceId RenderGraph::importImage(const char* name, ImageLayout initial_layout,
                                    ImageLayout final_layout)
{
    DeclaredResource declared;
    declared.plan.name = name;
    declared.plan.kind = ResourceKind::Image;
    declared.plan.imported = true;
    declared.plan.initial_layout = initial_layout;
    declared.plan.final_layout = final_layout;
    resources_.push_back(declared);
    return static_cast<ResourceId>(resources_.size() - 1U);
}

ResourceId RenderGraph::importBuffer(const char* name)
{
    DeclaredResource declared;
    declared.plan.name = name;
    declared.plan.kind = ResourceKind::Buffer;
    declared.plan.imported = true;
    resources_.push_back(declared);
    return static_cast<ResourceId>(resources_.size() - 1U);
}

void RenderGraph::addPass(const PassDesc& desc)
{
    DeclaredPass pass;
    pass.desc = desc;
    pass.desc.name = desc.name;
    for (std::uint32_t i = 0; i < desc.read_count; ++i)
    {
        pass.reads.push_back(DeclaredUse{desc.reads[i], false});
    }
    for (std::uint32_t i = 0; i < desc.write_count; ++i)
    {
        pass.writes.push_back(DeclaredUse{desc.writes[i], true});
    }
    passes_.push_back(std::move(pass));
}

void RenderGraph::exportResource(ResourceId resource)
{
    exports_.push_back(resource);
}

bool RenderGraph::compile(GraphPlan& out_plan, std::string& out_error,
                          PlatformTags target_tags) const
{
    out_plan = GraphPlan{};
    out_error.clear();

    /* Declaration checks first: every use names a resource, one use per
       resource per pass, and transient images know their format. */
    for (std::uint32_t pass_index = 0; pass_index < passes_.size(); ++pass_index)
    {
        const DeclaredPass& pass = passes_[pass_index];
        if (pass.desc.name == nullptr)
        {
            out_error = "a pass has no name";
            return false;
        }
        if ((pass.desc.tags & ~target_tags) != 0U)
        {
            /* Culled below with the dead passes; nothing to validate further
               would change that. */
            continue;
        }
        for (const std::vector<DeclaredUse>* list : {&pass.reads, &pass.writes})
        {
            for (const DeclaredUse& use : *list)
            {
                if (use.use.resource >= resources_.size())
                {
                    out_error =
                        std::string("pass '") + pass.desc.name + "' uses an undeclared resource";
                    return false;
                }
            }
        }
        /* One use per resource per pass: the derivation tracks one state
           change per contact, and two contacts would be two. */
        std::vector<ResourceId> touched;
        for (const std::vector<DeclaredUse>* list : {&pass.reads, &pass.writes})
        {
            for (const DeclaredUse& use : *list)
            {
                if (std::find(touched.begin(), touched.end(), use.use.resource) != touched.end())
                {
                    out_error =
                        std::string("pass '") + pass.desc.name + "' uses one resource twice";
                    return false;
                }
                touched.push_back(use.use.resource);
            }
        }
        for (const DeclaredUse& read : pass.reads)
        {
            for (const DeclaredUse& write : pass.writes)
            {
                if (read.use.resource == write.use.resource)
                {
                    out_error = std::string("pass '") + pass.desc.name +
                                "' reads and writes the same resource";
                    return false;
                }
            }
        }
    }
    for (const DeclaredResource& resource : resources_)
    {
        if (!resource.plan.imported && resource.plan.kind == ResourceKind::Image &&
            resource.plan.format == RESONATE_RENDER_FORMAT_UNKNOWN)
        {
            out_error = std::string("transient image '") + resource.plan.name + "' has no format";
            return false;
        }
    }
    for (ResourceId exported : exports_)
    {
        if (exported >= resources_.size())
        {
            out_error = "an export names an undeclared resource";
            return false;
        }
    }

    /* Culling: a pass is live when it writes something exported or something a
       live pass reads — and when the target carries its tags. Reverse
       fixpoint, because liveness flows backwards from the exports through the
       reads. */
    const std::uint32_t pass_count = static_cast<std::uint32_t>(passes_.size());
    std::vector<bool> live(pass_count, false);
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (std::uint32_t p = pass_count; p-- > 0U;)
        {
            if (live[p] || (passes_[p].desc.tags & ~target_tags) != 0U)
            {
                continue;
            }
            bool needed = false;
            for (const DeclaredUse& write : passes_[p].writes)
            {
                if (std::find(exports_.begin(), exports_.end(), write.use.resource) !=
                    exports_.end())
                {
                    needed = true;
                    break;
                }
                for (std::uint32_t q = 0; q < pass_count; ++q)
                {
                    if (!live[q])
                    {
                        continue;
                    }
                    for (const DeclaredUse& read : passes_[q].reads)
                    {
                        if (read.use.resource == write.use.resource)
                        {
                            needed = true;
                            break;
                        }
                    }
                    if (needed)
                    {
                        break;
                    }
                }
                if (needed)
                {
                    break;
                }
            }
            if (needed)
            {
                live[p] = true;
                changed = true;
            }
        }
    }

    /* The plan's pieces, in declaration order with the dead passes gone. */
    out_plan.resources.reserve(resources_.size());
    for (const DeclaredResource& resource : resources_)
    {
        out_plan.resources.push_back(resource.plan);
    }
    for (std::uint32_t p = 0; p < pass_count; ++p)
    {
        if (!live[p])
        {
            continue;
        }
        const DeclaredPass& pass = passes_[p];
        PlanPass planned;
        planned.name = pass.desc.name;
        planned.queue = pass.desc.queue;
        planned.context = pass.desc.context;
        planned.execute = pass.desc.execute;
        planned.reads.reserve(pass.reads.size());
        for (const DeclaredUse& use : pass.reads)
        {
            planned.reads.push_back(use.use);
        }
        planned.writes.reserve(pass.writes.size());
        for (const DeclaredUse& use : pass.writes)
        {
            planned.writes.push_back(use.use);
        }
        out_plan.passes.push_back(std::move(planned));
    }
    const std::uint32_t step_count = static_cast<std::uint32_t>(out_plan.passes.size());

    /* Lifetimes over the live order — the aliaser's and materializer's input.
       Uses are tracked by declaration index so a culled pass's resource reads
       no lifetime from it. */
    out_plan.lifetimes.assign(resources_.size(), ResourceLifetime{});
    for (std::uint32_t step = 0; step < step_count; ++step)
    {
        const PlanPass& pass = out_plan.passes[step];
        for (const std::vector<Use>* list : {&pass.reads, &pass.writes})
        {
            for (const Use& use : *list)
            {
                ResourceLifetime& lifetime = out_plan.lifetimes[use.resource];
                if (lifetime.first_step == UINT32_MAX)
                {
                    lifetime.first_step = step;
                }
                lifetime.last_step = step;
            }
        }
    }

    /* Alias groups: first-fit over the transient resources in declaration
       order, per kind, on disjoint lifetimes. Imports stay outside. */
    for (ResourceId resource = 0; resource < resources_.size(); ++resource)
    {
        const PlanResource& plan_resource = out_plan.resources[resource];
        if (plan_resource.imported)
        {
            continue;
        }
        const ResourceLifetime& lifetime = out_plan.lifetimes[resource];
        if (lifetime.first_step == UINT32_MAX)
        {
            continue;
        }
        AliasGroup* group = nullptr;
        for (AliasGroup& candidate : out_plan.aliases)
        {
            if (candidate.kind != plan_resource.kind)
            {
                continue;
            }
            bool overlaps = false;
            for (ResourceId member : candidate.members)
            {
                const ResourceLifetime& other = out_plan.lifetimes[member];
                if (lifetime.first_step <= other.last_step &&
                    other.first_step <= lifetime.last_step)
                {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps)
            {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr)
        {
            out_plan.aliases.push_back(AliasGroup{plan_resource.kind, {}});
            group = &out_plan.aliases.back();
        }
        group->members.push_back(resource);
    }

    /* Barriers: walk the live order tracking each resource's state, and emit
       the transition a use needs from the state before it. A write always
       costs a barrier against whatever came before; two reads of one layout
       cost nothing; a layout change costs one either way. */
    std::vector<TrackedState> states(resources_.size());
    for (ResourceId resource = 0; resource < resources_.size(); ++resource)
    {
        states[resource].layout = out_plan.resources[resource].initial_layout;
    }
    out_plan.steps.resize(step_count);
    for (std::uint32_t step = 0; step < step_count; ++step)
    {
        const PlanPass& pass = out_plan.passes[step];
        std::vector<Barrier>& barriers = out_plan.steps[step].barriers;
        out_plan.steps[step].pass = step;

        for (const std::vector<Use>* list : {&pass.reads, &pass.writes})
        {
            for (const Use& use : *list)
            {
                TrackedState& state = states[use.resource];
                const bool write = accessIsWrite(use.access);
                const bool needs_barrier =
                    !state.ever_used || state.layout != use.layout || state.last_was_write || write;
                if (needs_barrier)
                {
                    Barrier barrier;
                    barrier.resource = use.resource;
                    barrier.before_stages = state.stages;
                    barrier.before_access = state.access;
                    barrier.before_layout = state.layout;
                    barrier.after_stages = use.stages;
                    barrier.after_access = use.access;
                    barrier.after_layout = use.layout;
                    barriers.push_back(barrier);
                    state.stages = use.stages;
                    state.access = use.access;
                    state.layout = use.layout;
                }
                else
                {
                    /* Read after read: the stages accumulate, so the next
                       write barriers against every reader. */
                    state.stages |= use.stages;
                    state.access |= use.access;
                }
                state.ever_used = true;
                state.last_was_write = write;
            }
        }
    }

    /* Final barriers: an exported resource ends the frame in the state its
       importer declared — the backbuffer in present, ready for the swapchain. */
    for (ResourceId exported : exports_)
    {
        const TrackedState& state = states[exported];
        const ImageLayout final_layout = out_plan.resources[exported].final_layout;
        if (!state.ever_used || state.layout == final_layout)
        {
            continue;
        }
        Barrier barrier;
        barrier.resource = exported;
        barrier.before_stages = state.stages;
        barrier.before_access = state.access;
        barrier.before_layout = state.layout;
        barrier.after_layout = final_layout;
        out_plan.final_barriers.push_back(barrier);
    }
    return true;
}

} // namespace resonate::render
