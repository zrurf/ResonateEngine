#include <catch2/catch_test_macros.hpp>

#include <resonate/render/executor.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

using namespace resonate::render;

/* The executor is device-agnostic, so the tests run it against a device that
   only records what it was asked: the call sequence, the barrier payloads and
   the created handles are the assertions, and no GPU is involved. */
struct FakeDevice
{
    ResonateRenderDevice device{};
    std::vector<std::string> calls;
    std::vector<ResonateRenderBarrierDesc> barriers;
    std::vector<ResonateRenderImageDesc> created_images;
    uint64_t next_handle = 1;
    bool has_surface = true;

    ResonateStatus beginFrame()
    {
        calls.emplace_back("begin_frame");
        return has_surface ? RESONATE_OK : RESONATE_E_STATE;
    }

    ResonateStatus createImage(const ResonateRenderImageDesc* desc, ResonateRenderImage* out)
    {
        calls.emplace_back("create_image");
        created_images.push_back(*desc);
        *out = ResonateRenderImage{next_handle++};
        return RESONATE_OK;
    }

    ResonateStatus createBuffer(const ResonateRenderBufferDesc*, ResonateRenderBuffer* out)
    {
        calls.emplace_back("create_buffer");
        *out = ResonateRenderBuffer{next_handle++};
        return RESONATE_OK;
    }

    void destroyImage(ResonateRenderImage)
    {
        calls.emplace_back("destroy_image");
    }

    void destroyBuffer(ResonateRenderBuffer)
    {
        calls.emplace_back("destroy_buffer");
    }

    ResonateStatus barrier(const ResonateRenderBarrierDesc* desc)
    {
        calls.emplace_back("barrier");
        barriers.push_back(*desc);
        return RESONATE_OK;
    }

    ResonateStatus present()
    {
        calls.emplace_back("present");
        return RESONATE_OK;
    }
};

ResonateStatus fakeBeginFrame(void* self)
{
    return static_cast<FakeDevice*>(self)->beginFrame();
}

ResonateStatus fakeCreateImage(void* self, const ResonateRenderImageDesc* desc,
                               ResonateRenderImage* out)
{
    return static_cast<FakeDevice*>(self)->createImage(desc, out);
}

ResonateStatus fakeCreateBuffer(void* self, const ResonateRenderBufferDesc* desc,
                                ResonateRenderBuffer* out)
{
    return static_cast<FakeDevice*>(self)->createBuffer(desc, out);
}

void fakeDestroyImage(void* self, ResonateRenderImage image)
{
    static_cast<FakeDevice*>(self)->destroyImage(image);
}

void fakeDestroyBuffer(void* self, ResonateRenderBuffer buffer)
{
    static_cast<FakeDevice*>(self)->destroyBuffer(buffer);
}

ResonateStatus fakeBarrier(void* self, const ResonateRenderBarrierDesc* desc)
{
    return static_cast<FakeDevice*>(self)->barrier(desc);
}

ResonateStatus fakePresent(void* self)
{
    return static_cast<FakeDevice*>(self)->present();
}

FakeDevice* makeDevice()
{
    auto* fake = new FakeDevice();
    fake->device.header =
        RESONATE_CAPABILITY_HEADER_INIT(ResonateRenderDevice, RESONATE_THREAD_ANY);
    fake->device.self = fake;
    fake->device.begin_frame = &fakeBeginFrame;
    fake->device.create_image = &fakeCreateImage;
    fake->device.create_buffer = &fakeCreateBuffer;
    fake->device.destroy_image = &fakeDestroyImage;
    fake->device.destroy_buffer = &fakeDestroyBuffer;
    fake->device.barrier = &fakeBarrier;
    fake->device.present = &fakePresent;
    return fake;
}

/* A scene pass writing a transient, a post pass reading it into the canvas:
   the smallest graph with a materialized resource, interior barriers and a
   canvas the executor must not final-barrier. The passes record what the
   context resolved for them. */
struct DemoGraph
{
    RenderGraph graph;
    GraphPlan plan;
    ResourceId canvas = 0;
    ResourceId hdr = 0;
    int scene_draws = 0;
    int post_draws = 0;
    ResonateRenderImage seen_canvas{};
    ResonateRenderImage seen_hdr{};

    DemoGraph()
    {
        canvas = graph.importImage("backbuffer", kLayoutUndefined, kLayoutPresent);
        hdr = graph.declareImage("hdr", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, 64U, 64U,
                                 RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT);

        Use scene_write{};
        scene_write.resource = hdr;
        scene_write.stages = kStageColorOutput;
        scene_write.access = kAccessColorWrite;
        scene_write.layout = kLayoutColorAttachment;
        PassDesc scene{};
        scene.name = "scene";
        scene.writes = &scene_write;
        scene.write_count = 1U;
        scene.context = this;
        scene.execute = [](void* context, GraphExecuteContext&)
        { static_cast<DemoGraph*>(context)->scene_draws += 1; };
        graph.addPass(scene);

        Use hdr_read{};
        hdr_read.resource = hdr;
        hdr_read.stages = kStageFragment;
        hdr_read.access = kAccessShaderRead;
        hdr_read.layout = kLayoutShaderReadOnly;
        Use canvas_write{};
        canvas_write.resource = canvas;
        canvas_write.stages = kStageColorOutput;
        canvas_write.access = kAccessColorWrite;
        canvas_write.layout = kLayoutColorAttachment;
        Use post_reads[1] = {hdr_read};
        PassDesc post{};
        post.name = "post";
        post.reads = post_reads;
        post.read_count = 1U;
        post.writes = &canvas_write;
        post.write_count = 1U;
        post.context = this;
        post.execute = [](void* context, GraphExecuteContext& commands)
        {
            auto* self = static_cast<DemoGraph*>(context);
            self->post_draws += 1;
            self->seen_canvas = commands.image(self->canvas);
            self->seen_hdr = commands.image(self->hdr);
        };
        graph.addPass(post);

        graph.exportResource(canvas);
        std::string error;
        REQUIRE(graph.compile(plan, error));
    }
};

} // namespace

TEST_CASE("executor: one submission per frame, in plan order", "[render][executor]")
{
    DemoGraph demo;
    FakeDevice* fake = makeDevice();
    {
        GraphExecutor executor(fake->device, demo.plan, demo.canvas);

        std::string error;
        REQUIRE(executor.execute(error));
        REQUIRE(executor.execute(error));

        /* The transient materializes before the first frame opens (the resource
           table write is the creation); frame two reuses it. Each frame replays
           the plan's two transient barriers — the canvas's own are the device's
           and stay unreplayed. */
        REQUIRE(fake->calls.size() == 9U);
        CHECK(fake->calls[0] == "create_image");
        CHECK(fake->calls[1] == "begin_frame");
        CHECK(fake->calls[2] == "barrier");
        CHECK(fake->calls[3] == "barrier");
        CHECK(fake->calls[4] == "present");
        CHECK(fake->calls[5] == "begin_frame");
        CHECK(fake->calls[8] == "present");

        const auto first_present = std::find(fake->calls.begin(), fake->calls.end(), "present");
        REQUIRE(first_present != fake->calls.end());
        CHECK(std::count(fake->calls.begin(), first_present, "create_image") == 1);
        CHECK(fake->created_images.size() == 1U);
        CHECK(fake->created_images[0].width == 64U);

        CHECK(demo.scene_draws == 2);
        CHECK(demo.post_draws == 2);
    }
    delete fake;
}

TEST_CASE("executor: the canvas reads as {0}, the transient as its created handle",
          "[render][executor]")
{
    DemoGraph demo;
    FakeDevice* fake = makeDevice();
    {
        GraphExecutor executor(fake->device, demo.plan, demo.canvas);

        std::string error;
        REQUIRE(executor.execute(error));

        CHECK(demo.seen_canvas.value == 0U);
        REQUIRE(fake->created_images.size() == 1U);
        CHECK(demo.seen_hdr.value == 1U);

        /* The plan derives the canvas's present transition, but the device's own
           render pass takes the swapchain image there — the executor replays no
           barrier that lands in present. */
        bool canvas_final = false;
        for (const Barrier& barrier : demo.plan.final_barriers)
        {
            canvas_final = canvas_final || barrier.resource == demo.canvas;
        }
        CHECK(canvas_final);
        for (const ResonateRenderBarrierDesc& replayed : fake->barriers)
        {
            CHECK(replayed.after_layout != RESONATE_RENDER_LAYOUT_PRESENT);
        }
    }
    delete fake;
}

TEST_CASE("executor: a headless frame opens nothing and runs nothing", "[render][executor]")
{
    DemoGraph demo;
    FakeDevice* fake = makeDevice();
    fake->has_surface = false;
    {
        GraphExecutor executor(fake->device, demo.plan, demo.canvas);

        std::string error;
        CHECK_FALSE(executor.execute(error));
        /* The materialization still ran and the frame was still requested —
           the fake device records before refusing — but nothing opened and no
           pass ran. */
        REQUIRE(fake->calls.size() == 2U);
        CHECK(fake->calls[0] == "create_image");
        CHECK(fake->calls[1] == "begin_frame");
        CHECK(demo.scene_draws == 0);
    }
    delete fake;
}

TEST_CASE("executor: barriers carry the derived vocabulary", "[render][executor]")
{
    DemoGraph demo;
    FakeDevice* fake = makeDevice();
    {
        GraphExecutor executor(fake->device, demo.plan, demo.canvas);

        std::string error;
        REQUIRE(executor.execute(error));

        REQUIRE(fake->barriers.size() == 2U);
        /* The transient's acquire, then its write-to-read transition; the
           canvas's barriers are the device's and none is replayed for it. */
        CHECK(fake->barriers[0].image.value != 0U);
        CHECK(fake->barriers[0].before_layout == RESONATE_RENDER_LAYOUT_UNDEFINED);
        CHECK(fake->barriers[0].after_layout == RESONATE_RENDER_LAYOUT_COLOR_ATTACHMENT);
        CHECK(fake->barriers[0].after_stages == kStageColorOutput);
        CHECK(fake->barriers[1].before_layout == RESONATE_RENDER_LAYOUT_COLOR_ATTACHMENT);
        CHECK(fake->barriers[1].after_layout == RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY);
        CHECK(fake->barriers[1].before_access == kAccessColorWrite);
        CHECK(fake->barriers[1].after_access == kAccessShaderRead);
    }
    delete fake;
}

TEST_CASE("executor: destruction releases what it materialized", "[render][executor]")
{
    DemoGraph demo;
    FakeDevice* fake = makeDevice();
    {
        {
            GraphExecutor executor(fake->device, demo.plan, demo.canvas);
            std::string error;
            REQUIRE(executor.execute(error));
        }
        CHECK(std::count(fake->calls.begin(), fake->calls.end(), "destroy_image") == 1);
    }
    delete fake;
}
