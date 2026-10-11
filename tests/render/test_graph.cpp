#include <catch2/catch_test_macros.hpp>

#include <resonate/render/graph.h>

#include <string>
#include <vector>

namespace
{

using namespace resonate::render;

Use read(ResourceId resource, StageFlags stages, AccessFlags access, ImageLayout layout)
{
    return Use{resource, stages, access, layout};
}

Use write(ResourceId resource, StageFlags stages, AccessFlags access, ImageLayout layout)
{
    return Use{resource, stages, access, layout};
}

/* Finds the barrier a plan step carries for one resource, if any. */
const Barrier* barrierFor(const PlanStep& step, ResourceId resource)
{
    for (const Barrier& barrier : step.barriers)
    {
        if (barrier.resource == resource)
        {
            return &barrier;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("render graph: declaration order is the execution order", "[render][graph]")
{
    RenderGraph graph;
    const ResourceId backbuffer =
        graph.importImage("backbuffer", ImageLayout::Undefined, ImageLayout::Present);
    const ResourceId hdr =
        graph.declareImage("hdr", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, 1600U, 900U);

    const Use hdr_write =
        write(hdr, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"scene", Queue::Graphics, 0U, nullptr, 0U, &hdr_write, 1U, nullptr, nullptr});
    const Use hdr_read = read(hdr, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use back_write =
        write(backbuffer, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"post", Queue::Graphics, 0U, &hdr_read, 1U, &back_write, 1U, nullptr, nullptr});
    graph.exportResource(backbuffer);

    GraphPlan plan;
    std::string error;
    REQUIRE(graph.compile(plan, error));
    INFO(error);
    REQUIRE(plan.steps.size() == 2U);
    CHECK(plan.passes[0].name == std::string("scene"));
    CHECK(plan.passes[1].name == std::string("post"));
}

TEST_CASE("render graph: a pass nobody consumes is culled, and its feeders with it",
          "[render][graph]")
{
    RenderGraph graph;
    const ResourceId backbuffer =
        graph.importImage("backbuffer", ImageLayout::Undefined, ImageLayout::Present);
    const ResourceId hdr =
        graph.declareImage("hdr", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, 1600U, 900U);
    const ResourceId scratch =
        graph.declareImage("scratch", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, 64U, 64U);
    const ResourceId feed =
        graph.declareImage("feed", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, 64U, 64U);

    const Use hdr_write =
        write(hdr, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"scene", Queue::Graphics, 0U, nullptr, 0U, &hdr_write, 1U, nullptr, nullptr});
    /* Writes scratch, which nothing reads and nothing exports: dead. */
    const Use scratch_write =
        write(scratch, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"dead", Queue::Graphics, 0U, nullptr, 0U, &scratch_write, 1U, nullptr, nullptr});
    /* Reads feed, which only dead work produces: dead with it. */
    const Use feed_read =
        read(feed, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use scratch2_write =
        write(scratch, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass(
        {"dead-child", Queue::Graphics, 0U, &feed_read, 1U, &scratch2_write, 1U, nullptr, nullptr});
    const Use hdr_read = read(hdr, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use back_write =
        write(backbuffer, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"post", Queue::Graphics, 0U, &hdr_read, 1U, &back_write, 1U, nullptr, nullptr});
    graph.exportResource(backbuffer);

    GraphPlan plan;
    std::string error;
    REQUIRE(graph.compile(plan, error));
    INFO(error);
    REQUIRE(plan.steps.size() == 2U);
    CHECK(plan.passes[0].name == std::string("scene"));
    CHECK(plan.passes[1].name == std::string("post"));

    /* The culled resources have no lifetime; the live ones span their uses. */
    CHECK(plan.lifetimes[scratch].first_step == UINT32_MAX);
    CHECK(plan.lifetimes[hdr].first_step == 0U);
    CHECK(plan.lifetimes[hdr].last_step == 1U);
}

TEST_CASE("render graph: barriers come from the declarations", "[render][graph]")
{
    RenderGraph graph;
    const ResourceId backbuffer =
        graph.importImage("backbuffer", ImageLayout::Undefined, ImageLayout::Present);
    const ResourceId hdr =
        graph.declareImage("hdr", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, 1600U, 900U);

    const Use hdr_write =
        write(hdr, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"scene", Queue::Graphics, 0U, nullptr, 0U, &hdr_write, 1U, nullptr, nullptr});
    const Use hdr_read = read(hdr, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use back_write =
        write(backbuffer, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"post", Queue::Graphics, 0U, &hdr_read, 1U, &back_write, 1U, nullptr, nullptr});
    graph.exportResource(backbuffer);

    GraphPlan plan;
    std::string error;
    REQUIRE(graph.compile(plan, error));
    INFO(error);

    /* The first pass takes the transient image from undefined; the second
       reads it in another layout, so the write-to-read transition is here. */
    const Barrier* acquire = barrierFor(plan.steps[0], hdr);
    REQUIRE(acquire != nullptr);
    CHECK(acquire->before_layout == ImageLayout::Undefined);
    CHECK(acquire->after_layout == ImageLayout::ColorAttachment);
    CHECK(acquire->after_stages == kStageColorOutput);

    const Barrier* to_read = barrierFor(plan.steps[1], hdr);
    REQUIRE(to_read != nullptr);
    CHECK(to_read->before_layout == ImageLayout::ColorAttachment);
    CHECK(to_read->after_layout == ImageLayout::ShaderReadOnly);
    CHECK(to_read->before_access == kAccessColorWrite);
    CHECK(to_read->after_access == kAccessShaderRead);

    /* The backbuffer enters undefined and leaves for present. */
    const Barrier* back_acquire = barrierFor(plan.steps[1], backbuffer);
    REQUIRE(back_acquire != nullptr);
    CHECK(back_acquire->before_layout == ImageLayout::Undefined);
    CHECK(back_acquire->after_layout == ImageLayout::ColorAttachment);
    REQUIRE(plan.final_barriers.size() == 1U);
    CHECK(plan.final_barriers[0].resource == backbuffer);
    CHECK(plan.final_barriers[0].before_layout == ImageLayout::ColorAttachment);
    CHECK(plan.final_barriers[0].after_layout == ImageLayout::Present);
}

TEST_CASE("render graph: read after read costs nothing, war does", "[render][graph]")
{
    RenderGraph graph;
    const ResourceId image =
        graph.declareImage("shared", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, 64U, 64U);
    const ResourceId other =
        graph.declareImage("other", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, 64U, 64U);

    const Use image_write =
        write(image, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass(
        {"produce", Queue::Graphics, 0U, nullptr, 0U, &image_write, 1U, nullptr, nullptr});
    const Use first_read =
        read(image, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use other_write_a =
        write(other, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass(
        {"read-a", Queue::Graphics, 0U, &first_read, 1U, &other_write_a, 1U, nullptr, nullptr});
    const Use second_read =
        read(image, kStageCompute, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use other_write_b =
        write(other, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass(
        {"read-b", Queue::Compute, 0U, &second_read, 1U, &other_write_b, 1U, nullptr, nullptr});
    const Use other_read =
        read(other, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use over_write =
        write(image, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    const Use over[1] = {other_read};
    graph.addPass({"over", Queue::Graphics, 0U, over, 1U, &over_write, 1U, nullptr, nullptr});
    graph.exportResource(image);

    GraphPlan plan;
    std::string error;
    REQUIRE(graph.compile(plan, error));
    INFO(error);
    REQUIRE(plan.steps.size() == 4U);

    /* read-a pays the write-to-read transition; read-b is a second reader of
       the same layout and pays nothing. */
    CHECK(barrierFor(plan.steps[1], image) != nullptr);
    CHECK(barrierFor(plan.steps[2], image) == nullptr);

    /* The write over two readers barriers against both: the accumulated read
       stages and accesses are what it transitions from. */
    const Barrier* war = barrierFor(plan.steps[3], image);
    REQUIRE(war != nullptr);
    CHECK(war->before_stages == (kStageFragment | kStageCompute));
    CHECK(war->before_access == kAccessShaderRead);
    CHECK(war->after_access == kAccessColorWrite);
}

TEST_CASE("render graph: disjoint lifetimes alias, overlapping ones and imports do not",
          "[render][graph]")
{
    RenderGraph graph;
    const ResourceId backbuffer =
        graph.importImage("backbuffer", ImageLayout::Undefined, ImageLayout::Present);
    const ResourceId hdr =
        graph.declareImage("hdr", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, 1600U, 900U);
    const ResourceId scratch_a =
        graph.declareImage("a", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, 64U, 64U);
    const ResourceId scratch_b =
        graph.declareImage("b", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, 64U, 64U);
    const ResourceId overlap =
        graph.declareImage("overlap", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, 64U, 64U);
    const ResourceId scratch_buffer = graph.declareBuffer("data", 1024U);

    /* a lives [0, 1]; b lives [2, 3]: disjoint, so they share backing. overlap
       lives [1, 2] and collides with both. The buffer is its own kind. */
    const Use a_write =
        write(scratch_a, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"produce-a", Queue::Graphics, 0U, nullptr, 0U, &a_write, 1U, nullptr, nullptr});
    const Use a_read =
        read(scratch_a, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use hdr_write =
        write(hdr, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    const Use overlap_write =
        write(overlap, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    const Use buffer_write =
        write(scratch_buffer, kStageCompute, kAccessShaderWrite, ImageLayout::Undefined);
    graph.addPass({"mid", Queue::Graphics, 0U, &a_read, 1U, &hdr_write, 1U, nullptr, nullptr});
    const Use overlap_read =
        read(overlap, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use b_write =
        write(scratch_b, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    const Use produce_b_reads[1] = {overlap_read};
    graph.addPass(
        {"produce-b", Queue::Graphics, 0U, produce_b_reads, 1U, &b_write, 1U, nullptr, nullptr});
    const Use hdr_read = read(hdr, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use buffer_read =
        read(scratch_buffer, kStageCompute, kAccessShaderRead, ImageLayout::Undefined);
    const Use b_read =
        read(scratch_b, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use reads[3] = {hdr_read, buffer_read, b_read};
    const Use back_write =
        write(backbuffer, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"post", Queue::Graphics, 0U, reads, 3U, &back_write, 1U, nullptr, nullptr});
    graph.exportResource(backbuffer);

    GraphPlan plan;
    std::string error;
    REQUIRE(graph.compile(plan, error));
    INFO(error);
    REQUIRE(plan.steps.size() == 4U);

    const AliasGroup* shared = nullptr;
    std::uint32_t solo_images = 0U;
    std::uint32_t buffer_groups = 0U;
    for (const AliasGroup& group : plan.aliases)
    {
        if (group.kind == ResourceKind::Buffer)
        {
            buffer_groups += 1U;
            continue;
        }
        if (group.members.size() > 1U)
        {
            shared = &group;
        }
        else
        {
            solo_images += 1U;
        }
    }
    REQUIRE(shared != nullptr);
    REQUIRE(shared->members.size() == 2U);
    CHECK(solo_images == 2U); /* hdr spans the frame; overlap collides with a and b */
    CHECK(buffer_groups == 1U);

    bool imported_aliased = false;
    for (const AliasGroup& group : plan.aliases)
    {
        for (ResourceId member : group.members)
        {
            imported_aliased = imported_aliased || member == backbuffer;
        }
    }
    CHECK_FALSE(imported_aliased);
}

TEST_CASE("render graph: passes the target's tags do not carry are culled", "[render][graph]")
{
    RenderGraph graph;
    const ResourceId backbuffer =
        graph.importImage("backbuffer", ImageLayout::Undefined, ImageLayout::Present);
    const ResourceId hdr =
        graph.declareImage("hdr", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, 1600U, 900U);
    const ResourceId shadows =
        graph.declareImage("shadows", RESONATE_RENDER_FORMAT_D32_SFLOAT, 2048U, 2048U);

    const Use shadow_write =
        write(shadows, kStageCompute, kAccessShaderWrite, ImageLayout::ShaderReadOnly);
    graph.addPass({"mobile-shadows", Queue::Compute, kTagHandheld, nullptr, 0U, &shadow_write, 1U,
                   nullptr, nullptr});
    const Use hdr_write =
        write(hdr, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    const Use shadow_read =
        read(shadows, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use reads[1] = {shadow_read};
    graph.addPass(
        {"scene", Queue::Graphics, kTagDesktop, reads, 1U, &hdr_write, 1U, nullptr, nullptr});
    const Use hdr_read = read(hdr, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
    const Use back_write =
        write(backbuffer, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"post", Queue::Graphics, 0U, &hdr_read, 1U, &back_write, 1U, nullptr, nullptr});
    graph.exportResource(backbuffer);

    /* A desktop target: the handheld-tagged shadow pass is the dead one —
       nothing live reads what it writes. */
    GraphPlan desktop;
    std::string error;
    REQUIRE(graph.compile(desktop, error, kTagDesktop));
    INFO(error);
    REQUIRE(desktop.steps.size() == 2U);
    CHECK(desktop.passes[0].name == std::string("scene"));
    CHECK(desktop.passes[1].name == std::string("post"));

    /* A handheld target: the desktop-tagged scene cannot run, and the shadow
       pass dies with its only consumer; untagged post still runs. */
    GraphPlan handheld;
    REQUIRE(graph.compile(handheld, error, kTagHandheld));
    INFO(error);
    REQUIRE(handheld.steps.size() == 1U);
    CHECK(handheld.passes[0].name == std::string("post"));
}

TEST_CASE("render graph: declaration errors are named, not guessed at", "[render][graph]")
{
    RenderGraph graph;
    const ResourceId image =
        graph.declareImage("image", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, 64U, 64U);

    const Use undeclared =
        write(99U, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
    graph.addPass({"bad", Queue::Graphics, 0U, nullptr, 0U, &undeclared, 1U, nullptr, nullptr});
    GraphPlan plan;
    std::string error;
    CHECK_FALSE(graph.compile(plan, error));
    CHECK(error.find("undeclared resource") != std::string::npos);
}

TEST_CASE("render graph: compilation is deterministic", "[render][graph]")
{
    auto build = []()
    {
        RenderGraph graph;
        const ResourceId backbuffer =
            graph.importImage("backbuffer", ImageLayout::Undefined, ImageLayout::Present);
        const ResourceId hdr =
            graph.declareImage("hdr", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, 1600U, 900U);
        const Use hdr_write =
            write(hdr, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
        graph.addPass(
            {"scene", Queue::Graphics, 0U, nullptr, 0U, &hdr_write, 1U, nullptr, nullptr});
        const Use hdr_read =
            read(hdr, kStageFragment, kAccessShaderRead, ImageLayout::ShaderReadOnly);
        const Use back_write =
            write(backbuffer, kStageColorOutput, kAccessColorWrite, ImageLayout::ColorAttachment);
        graph.addPass(
            {"post", Queue::Graphics, 0U, &hdr_read, 1U, &back_write, 1U, nullptr, nullptr});
        graph.exportResource(backbuffer);
        return graph;
    };

    GraphPlan first;
    GraphPlan second;
    std::string error;
    REQUIRE(build().compile(first, error));
    REQUIRE(build().compile(second, error));
    REQUIRE(first.steps.size() == second.steps.size());
    for (std::size_t step = 0; step < first.steps.size(); ++step)
    {
        REQUIRE(first.steps[step].barriers.size() == second.steps[step].barriers.size());
        for (std::size_t barrier = 0; barrier < first.steps[step].barriers.size(); ++barrier)
        {
            const Barrier& a = first.steps[step].barriers[barrier];
            const Barrier& b = second.steps[step].barriers[barrier];
            CHECK(a.resource == b.resource);
            CHECK(a.before_layout == b.before_layout);
            CHECK(a.after_layout == b.after_layout);
        }
    }
}
