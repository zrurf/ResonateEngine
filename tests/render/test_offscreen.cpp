#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/core/math.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/module/host.hpp>
#include <resonate/render/device.h>
#include <resonate/render/offscreen.hpp>
#include <resonate/renderer/renderer.h>
#include <resonate/ui/layout.h>

#include "paths.h"
#include "resonate.render/components.gen.h"
#include "resonate.ui/components.gen.h"

namespace
{

using resonate::Mat4;
using resonate::Vec3;

Mat4 translation(float x, float y, float z)
{
    Mat4 matrix{};
    matrix.m[0][0] = 1.0F;
    matrix.m[1][1] = 1.0F;
    matrix.m[2][2] = 1.0F;
    matrix.m[3][3] = 1.0F;
    matrix.m[0][3] = x;
    matrix.m[1][3] = y;
    matrix.m[2][3] = z;
    return matrix;
}

std::uint32_t lumaAt(const std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t x,
                     std::uint32_t y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4U;
    return (2126U * pixels[index] + 7152U * pixels[index + 1U] + 722U * pixels[index + 2U]) /
           10000U;
}

} // namespace

/* The offscreen target is the M2 acceptance path: the same renderer graph, the
   same device, no window — and the pixels are readable, which is what makes the
   GPU path testable at all. */
TEST_CASE("offscreen: the renderer draws into a device target and reads back",
          "[render][offscreen]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    INFO("the plugin targets are a build dependency of this test target");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 320U, 240U) == RESONATE_OK);
    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    auto* device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
        host->capabilities(), resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(),
        1U, nullptr));
    REQUIRE(device != nullptr);

    /* A camera on the axis looking at a cube 7 m ahead, and a light head-on to
       it: a small, deterministic scene with a bright center and a dark sky. */
    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));

    const auto camera = world.create();
    world.add(camera, resonate::hierarchy::WorldTransform{translation(0.0F, 2.5F, -7.0F)});
    world.add(camera, resonate::render::Camera{});

    const auto cube = world.create();
    world.add(cube, resonate::hierarchy::WorldTransform{translation(0.0F, 2.5F, 0.0F)});
    world.add(cube, resonate::render::MeshInstance{1U, Vec3{1.0F, 1.0F, 1.0F}});
    world.add(cube, resonate::render::Material{0U, Vec3{1.0F, 1.0F, 1.0F}});

    const auto sun = world.create();
    world.add(sun, resonate::render::DirectionalLight{Vec3{0.0F, 0.0F, 1.0F},
                                                      Vec3{1.0F, 1.0F, 1.0F}, 2.0F});

    /* A red overlay quadrant over the top-left corner: the UI pass and the
       composite's blend are visible in the readback. */
    const auto ui_canvas = world.create();
    world.add(ui_canvas, resonate::ui::Canvas{});
    world.add(ui_canvas, resonate::ui::ComputedRect{});
    const auto ui_quad = world.create();
    resonate::ui::RectTransform ui_rect;
    ui_rect.x = 8.0F;
    ui_rect.y = 8.0F;
    ui_rect.width = 140.0F;
    ui_rect.height = 48.0F;
    world.add(ui_quad, ui_rect);
    world.add(ui_quad, resonate::ui::ComputedRect{});
    world.add(ui_quad, resonate::ui::Background{resonate::Vec4{1.0F, 0.0F, 0.0F, 1.0F}});

    /* A half-transparent blue over the red half: the overlay target keeps
       straight-alpha color, so the composite shows the blend. */
    const auto ui_blend = world.create();
    resonate::ui::RectTransform blend_rect;
    blend_rect.x = 80.0F;
    blend_rect.y = 8.0F;
    blend_rect.width = 60.0F;
    blend_rect.height = 48.0F;
    world.add(ui_blend, blend_rect);
    world.add(ui_blend, resonate::ui::ComputedRect{});
    world.add(ui_blend, resonate::ui::Background{resonate::Vec4{0.0F, 0.0F, 1.0F, 0.5F}});
    {
        resonate::ecs::CommandBuffer commands(world);
        resonate::hierarchy::setParent(commands, ui_quad, ui_canvas);
        resonate::hierarchy::setParent(commands, ui_blend, ui_canvas);
        world.play(commands);
    }
    REQUIRE(resonate::ui::layout(world, ui_canvas, 320.0F, 240.0F));

    resonate::renderer::RenderSystem render_system;
    render_system.setRegistry(host->capabilities());
    /* The scene pass renders at 4x and resolves into the HDR target: the whole
       graph runs on the device's transient MSAA path (R1). */
    /* RESONATE_MSAA opts a run into the MSAA path for side-by-side runs; the
       suite stays at 1x until the scene shading under a resolve is settled. */
    const char* msaa_env = std::getenv("RESONATE_MSAA");
    render_system.setMsaa(msaa_env != nullptr ? static_cast<std::uint32_t>(std::atoi(msaa_env))
                                              : 0U);
    for (int frame = 0; frame < 4; ++frame)
    {
        render_system.run(world);
        render_system.waitIdle();
    }
    /* The render thread owns the device calls; the readback is a main-thread
       call, so the handoff has to be drained first. */
    render_system.waitIdle();

    std::vector<std::uint8_t> pixels(320U * 240U * 4U);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const ResonateStatus status = device->readback_canvas(
        device->self, pixels.data(), static_cast<std::uint64_t>(pixels.size()), &width, &height);

    /* A machine without a Vulkan device cannot answer; the rest of the suite
       still covers the CPU side. */
    if (status != RESONATE_OK)
    {
        render_system.release();
        SKIP("no Vulkan offscreen device on this machine");
    }
    REQUIRE(width == 320U);
    REQUIRE(height == 240U);

    if (const char* dump = std::getenv("RESONATE_DUMP_OFFSCREEN"); dump != nullptr)
    {
        std::FILE* out = std::fopen(dump, "wb");
        const std::uint32_t stride = (width * 3U + 3U) & ~3U;
        const std::uint32_t size = 54U + stride * height;
        unsigned char header[54] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
                                    40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const auto put32 = [&header](std::size_t at, std::uint32_t value)
        {
            for (int byte = 0; byte < 4; ++byte)
            {
                header[at + static_cast<std::size_t>(byte)] =
                    static_cast<unsigned char>((value >> (8U * byte)) & 0xFFU);
            }
        };
        put32(2U, size);
        put32(18U, width);
        put32(22U, height);
        put32(28U, 24U);
        put32(34U, stride * height);
        std::fwrite(header, 1, sizeof(header), out);
        std::vector<unsigned char> row(stride, 0U);
        for (std::uint32_t y = 0U; y < height; ++y)
        {
            const std::uint8_t* source =
                pixels.data() + static_cast<std::size_t>(height - 1U - y) * width * 4U;
            for (std::uint32_t x = 0U; x < width; ++x)
            {
                row[x * 3U] = source[x * 4U + 2U];
                row[x * 3U + 1U] = source[x * 4U + 1U];
                row[x * 3U + 2U] = source[x * 4U];
            }
            std::fwrite(row.data(), 1, row.size(), out);
        }
        std::fclose(out);
    }

    const std::uint32_t sky = lumaAt(pixels, width, 4U, 4U);
    const std::uint32_t center = lumaAt(pixels, width, width / 2U, height / 2U);
    INFO("center=" << center << " sky=" << sky);
    CHECK(center > sky + 40U);

    /* The cube covers a meaningful area of the frame, not a stray pixel. */
    std::uint32_t lit = 0;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            lit += lumaAt(pixels, width, x, y) > sky + 40U ? 1U : 0U;
        }
    }
    INFO("lit=" << lit);
    CHECK(lit > 400U);

    /* The overlay is opaque red inside its rectangle. */
    const std::size_t ui_index = (static_cast<std::size_t>(20U) * width + 20U) * 4U;
    CHECK(pixels[ui_index] > 200U);
    CHECK(pixels[ui_index + 1U] < 40U);
    CHECK(pixels[ui_index + 2U] < 40U);

    /* Half-transparent blue over the red is purple: the UI pass blended the
       second quad into the first and the composite saw the result. The canvas
       is sRGB, so the linear 0.5 lands around 188 in the readback. */
    const std::size_t blend_index = (static_cast<std::size_t>(30U) * width + 110U) * 4U;
    CHECK(pixels[blend_index] > 160U);
    CHECK(pixels[blend_index] < 220U);
    CHECK(pixels[blend_index + 1U] < 40U);
    CHECK(pixels[blend_index + 2U] > 160U);
    CHECK(pixels[blend_index + 2U] < 220U);

    render_system.release();
    host->detachAll();
}

/* Settings arrive from the console thread while the render thread owns the
   device: each change is applied between frames, and a change that alters the
   graph retires the old one instead of indexing a resource it never made. */
TEST_CASE("offscreen: live render settings apply between frames", "[render][offscreen]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 128U, 96U) == RESONATE_OK);
    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    auto* device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
        host->capabilities(), resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(),
        1U, nullptr));
    REQUIRE(device != nullptr);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));

    const auto camera = world.create();
    world.add(camera, resonate::hierarchy::WorldTransform{translation(0.0F, 2.5F, -7.0F)});
    world.add(camera, resonate::render::Camera{});
    const auto cube = world.create();
    world.add(cube, resonate::hierarchy::WorldTransform{translation(0.0F, 2.5F, 0.0F)});
    world.add(cube, resonate::render::MeshInstance{1U, Vec3{1.0F, 1.0F, 1.0F}});
    world.add(cube, resonate::render::Material{0U, Vec3{1.0F, 1.0F, 1.0F}});
    const auto sun = world.create();
    world.add(sun, resonate::render::DirectionalLight{Vec3{0.0F, 0.0F, 1.0F},
                                                      Vec3{1.0F, 1.0F, 1.0F}, 2.0F});

    resonate::renderer::RenderSystem render_system;
    render_system.setRegistry(host->capabilities());
    std::string error;
    resonate::renderer::FeatureStack stack;
    REQUIRE(resonate::renderer::parseFeatureStack("tonemap", "none", stack, error));
    render_system.setFeatures(stack);
    render_system.warmup();

    const auto draw = [&](int frames)
    {
        for (int frame = 0; frame < frames; ++frame)
        {
            render_system.run(world);
            render_system.waitIdle();
        }
    };
    draw(2);

    /* Enable bloom and FXAA on a graph built without them. */
    REQUIRE(resonate::renderer::parseFeatureStack("bloom, tonemap", "fxaa", stack, error));
    render_system.setFeatures(stack);
    draw(2);

    /* Rebuild the scene targets while frames are in flight. */
    render_system.setMsaa(4);
    REQUIRE(render_system.setHdrFormat("rgba16f", error));
    render_system.setShadowSize(1024);
    render_system.setPresentMode("fifo");
    draw(2);

    std::vector<std::uint8_t> pixels(128U * 96U * 4U);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const ResonateStatus status = device->readback_canvas(
        device->self, pixels.data(), static_cast<std::uint64_t>(pixels.size()), &width, &height);
    render_system.release();
    host->detachAll();
    if (status != RESONATE_OK)
    {
        SKIP("no Vulkan offscreen device on this machine");
    }
    CHECK(width == 128U);
    CHECK(height == 96U);
}

/* The pass identity's action half (R5): what an attachment loads and stores is
   the caller's choice per pass, and a pipeline built for the defaults runs in
   every such pass. The readback proves the load really kept the frame the
   pass before it left. */
TEST_CASE("offscreen: a loading pass keeps what the frame before it left",
          "[render][offscreen]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 64U, 64U) == RESONATE_OK);
    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    auto* device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
        host->capabilities(), resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(),
        1U, nullptr));
    REQUIRE(device != nullptr);
    /* Raw device frames: the renderer normally attaches the target, so this
       test does it itself. */
    REQUIRE(device->attach_offscreen(device->self, 64U, 64U) == RESONATE_OK);

    const auto frame = [&](float red, float green, float blue, std::uint32_t ops)
    {
        REQUIRE(device->begin_frame(device->self) == RESONATE_OK);
        ResonateRenderPassDesc pass = {};
        pass.struct_size = sizeof(ResonateRenderPassDesc);
        pass.color.value = 0U; /* the canvas */
        pass.clear_color = ResonateVec4{red, green, blue, 1.0F};
        pass.attachment_ops = ops;
        REQUIRE(device->begin_pass(device->self, &pass) == RESONATE_OK);
        REQUIRE(device->end_pass(device->self) == RESONATE_OK);
        REQUIRE(device->present(device->self) == RESONATE_OK);
        REQUIRE(device->wait_idle(device->self) == RESONATE_OK);
    };
    const auto read_red = [&]
    {
        std::vector<std::uint8_t> pixels(64U * 64U * 4U);
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        REQUIRE(device->readback_canvas(device->self, pixels.data(),
                                        static_cast<std::uint64_t>(pixels.size()), &width,
                                        &height) == RESONATE_OK);
        return pixels[0];
    };

    /* A clearing pass really clears. Two identical frames: after a single
       presented frame the readback's slot choice is the device's business, and
       the second frame makes the read deterministic for the test. */
    frame(1.0F, 0.0F, 0.0F, 0U);
    frame(1.0F, 0.0F, 0.0F, 0U);
    CHECK(read_red() > 200U);

    /* A loading pass ignores its clear color and keeps the frame before it. */
    frame(0.0F, 0.0F, 0.0F, 1U);
    CHECK(read_red() > 200U);

    /* Back to the default: the clear wins again. */
    frame(0.0F, 0.0F, 0.0F, 0U);
    CHECK(read_red() < 40U);

    host->detachAll();
}

/* The device-owned transient MSAA (R1, the Filament shape): a samples>1 pass
   renders on the device's 4x attachments and resolves into its 1x color
   target, so a bare clearing pass still lands its color in the readback -
   through the resolve, not the clear. */
TEST_CASE("offscreen: a multisampled pass resolves its clear into the canvas",
          "[render][offscreen]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 64U, 64U) == RESONATE_OK);
    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    auto* device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
        host->capabilities(), resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(),
        1U, nullptr));
    REQUIRE(device != nullptr);
    REQUIRE(device->attach_offscreen(device->self, 64U, 64U) == RESONATE_OK);

    const auto frame = [&](float red, std::uint32_t samples)
    {
        REQUIRE(device->begin_frame(device->self) == RESONATE_OK);
        ResonateRenderPassDesc pass = {};
        pass.struct_size = sizeof(ResonateRenderPassDesc);
        pass.color.value = 0U; /* the canvas */
        pass.clear_color = ResonateVec4{red, 0.0F, 0.0F, 1.0F};
        pass.samples = samples;
        REQUIRE(device->begin_pass(device->self, &pass) == RESONATE_OK);
        REQUIRE(device->end_pass(device->self) == RESONATE_OK);
        REQUIRE(device->present(device->self) == RESONATE_OK);
        REQUIRE(device->wait_idle(device->self) == RESONATE_OK);
    };
    const auto read_red = [&]
    {
        std::vector<std::uint8_t> pixels(64U * 64U * 4U);
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        REQUIRE(device->readback_canvas(device->self, pixels.data(),
                                        static_cast<std::uint64_t>(pixels.size()), &width,
                                        &height) == RESONATE_OK);
        return pixels[0];
    };

    frame(1.0F, 4U);
    frame(1.0F, 4U);
    CHECK(read_red() > 200U);

    frame(0.0F, 4U);
    frame(0.0F, 4U);
    CHECK(read_red() < 40U);

    host->detachAll();
}

/* The R1 sample anomaly isolated on the raw device path: the same white
   texture, the same table slot, the same fixed-UV read, rendered by the same
   probe pipeline into 1x and samples>1 passes. The renderer is not involved;
   a mismatch here is the device (or the hook stack under it), not the frame. */
TEST_CASE("offscreen: the probe pipeline reads the same texel at 1x and 4x",
          "[render][offscreen]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    const std::string shader_directory = resonate::test::findBuildDirectory("shaders");
    REQUIRE_FALSE(plugin_directory.empty());
    REQUIRE_FALSE(shader_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 64U, 64U) == RESONATE_OK);
    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    auto* device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
        host->capabilities(), resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(),
        1U, nullptr));
    REQUIRE(device != nullptr);
    REQUIRE(device->attach_offscreen(device->self, 64U, 64U) == RESONATE_OK);

    const auto read_file = [](const std::string& path, std::vector<std::uint8_t>& out)
    {
        std::FILE* in = std::fopen(path.c_str(), "rb");
        REQUIRE(in != nullptr);
        std::fseek(in, 0, SEEK_END);
        out.resize(static_cast<std::size_t>(std::ftell(in)));
        std::fseek(in, 0, SEEK_SET);
        const std::size_t read = std::fread(out.data(), 1, out.size(), in);
        std::fclose(in);
        REQUIRE(read == out.size());
    };

    std::vector<std::uint8_t> vertex_spirv;
    std::vector<std::uint8_t> fragment_spirv;
    read_file(shader_directory + "/probe.probeVertex.spv", vertex_spirv);
    read_file(shader_directory + "/probe.probeFragment.spv", fragment_spirv);

    ResonateRenderShader vertex = {};
    ResonateRenderShader fragment = {};
    REQUIRE(device->create_shader(device->self, RESONATE_RENDER_SHADER_VERTEX, vertex_spirv.data(),
                                  vertex_spirv.size(), &vertex) == RESONATE_OK);
    REQUIRE(device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT,
                                  fragment_spirv.data(), fragment_spirv.size(),
                                  &fragment) == RESONATE_OK);

    /* The white 1x1 texture through the same table path the renderer uses. */
    ResonateRenderImageDesc image = {};
    image.struct_size = sizeof(image);
    image.format = RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM;
    image.width = 1U;
    image.height = 1U;
    image.usage = RESONATE_RENDER_IMAGE_SAMPLED;
    ResonateRenderImage texture = {};
    REQUIRE(device->create_image(device->self, &image, &texture) == RESONATE_OK);
    const std::uint8_t white[4] = {255U, 255U, 255U, 255U};
    REQUIRE(device->upload_image(device->self, texture, white, sizeof(white), 4U) == RESONATE_OK);
    REQUIRE(device->bind_image_slot(device->self, 0U, texture, 0U) == RESONATE_OK);
    /* A second texture rebound every frame, the way the renderer rebinds the
       font atlas: its slot sits next to the probe's. */
    ResonateRenderImageDesc second = {};
    second.struct_size = sizeof(second);
    second.format = RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM;
    second.width = 1U;
    second.height = 1U;
    second.usage = RESONATE_RENDER_IMAGE_SAMPLED;
    ResonateRenderImage other = {};
    REQUIRE(device->create_image(device->self, &second, &other) == RESONATE_OK);
    const std::uint8_t gray[4] = {128U, 128U, 128U, 255U};
    REQUIRE(device->upload_image(device->self, other, gray, sizeof(gray), 4U) == RESONATE_OK);

    /* One pipeline per sample count: a samples>1 pipeline may only run in a
       samples>1 pass. */
    /* The cluster compute runs before the scene pass in the renderer's frame:
       a compute dispatch over the table's storage slots, same command buffer. */
    std::vector<std::uint8_t> compute_spirv;
    read_file(shader_directory + "/probe.probeCompute.spv", compute_spirv);
    ResonateRenderShader compute = {};
    REQUIRE(device->create_shader(device->self, RESONATE_RENDER_SHADER_COMPUTE,
                                  compute_spirv.data(), compute_spirv.size(),
                                  &compute) == RESONATE_OK);
    ResonateRenderBufferDesc storage_desc = {};
    storage_desc.struct_size = sizeof(storage_desc);
    storage_desc.size_bytes = 256U;
    storage_desc.usage = RESONATE_RENDER_BUFFER_STORAGE;
    ResonateRenderBuffer storage = {};
    REQUIRE(device->create_buffer(device->self, &storage_desc, &storage) == RESONATE_OK);
    const std::uint32_t storage_slot = device->buffer_slot(device->self, storage);

    ResonateRenderPipeline probes[2] = {}; /* [0] = 1x, [1] = 4x */
    for (int variant = 0; variant < 2; ++variant)
    {
        ResonateRenderGraphicsPipelineDesc pipeline = {};
        pipeline.struct_size = sizeof(pipeline);
        pipeline.vertex_shader = vertex;
        pipeline.fragment_shader = fragment;
        pipeline.color_format = RESONATE_RENDER_FORMAT_UNKNOWN; /* the canvas */
        pipeline.samples = variant == 0 ? 1U : 4U;
        REQUIRE(device->create_pipeline(device->self, &pipeline,
                                        &probes[variant]) == RESONATE_OK);
    }
    /* The table layout comes with the first graphics pipeline; a compute
       pipeline shares it. */
    ResonateRenderPipeline probe_compute = {};
    REQUIRE(device->create_compute_pipeline(device->self, compute, &probe_compute) == RESONATE_OK);

    /* A depth-only pass to run before the probed one: the renderer opens
       every frame with 1x colorless shadow passes and then draws the scene at
       the frame's sample count, so the command buffer mixes pass shapes. */
    ResonateRenderImageDesc shadow_image = {};
    shadow_image.struct_size = sizeof(shadow_image);
    shadow_image.format = RESONATE_RENDER_FORMAT_D32_SFLOAT;
    shadow_image.width = 64U;
    shadow_image.height = 64U;
    shadow_image.usage = RESONATE_RENDER_IMAGE_DEPTH_ATTACHMENT;
    ResonateRenderImage shadow = {};
    REQUIRE(device->create_image(device->self, &shadow_image, &shadow) == RESONATE_OK);

    /* The frame-data storage the probe's fragment reads: the sun term at the
       scene's offset. */
    ResonateRenderBufferDesc frame_desc = {};
    frame_desc.struct_size = sizeof(frame_desc);
    frame_desc.size_bytes = 1024U;
    frame_desc.usage = RESONATE_RENDER_BUFFER_STORAGE;
    ResonateRenderBuffer frame_buffer = {};
    REQUIRE(device->create_buffer(device->self, &frame_desc, &frame_buffer) == RESONATE_OK);
    std::vector<std::uint8_t> frame_bytes(1024U, 0U);
    const float sun[4] = {1.0F, 1.0F, 1.0F, 2.0F};
    /* view_proj 64 + camera_world 64 + camera_and_near 16 + direction_and_far 16 */
    std::memcpy(frame_bytes.data() + 160U, sun, sizeof(sun));
    const std::uint32_t frame_slot = device->buffer_slot(device->self, frame_buffer);

    const auto frame = [&](std::uint32_t samples)
    {
        REQUIRE(device->begin_frame(device->self) == RESONATE_OK);
        /* The renderer uploads the frame data inside the open frame; the
           update lands with the frame's own command stream. */
        REQUIRE(device->update_buffer(device->self, frame_buffer, 0U, frame_bytes.data(),
                                      static_cast<std::uint64_t>(frame_bytes.size())) ==
                RESONATE_OK);
        ResonateRenderPassDesc shadow_pass = {};
        shadow_pass.struct_size = sizeof(shadow_pass);
        shadow_pass.colorless = 1U;
        shadow_pass.depth = shadow;
        REQUIRE(device->begin_pass(device->self, &shadow_pass) == RESONATE_OK);
        REQUIRE(device->end_pass(device->self) == RESONATE_OK);
        REQUIRE(device->bind_image_slot(device->self, 1U, other, 1U) == RESONATE_OK);
        REQUIRE(device->bind_pipeline(device->self, probe_compute) == RESONATE_OK);
        const std::uint32_t compute_constants[5] = {storage_slot, 0U, 0U, 0U, 2U};
        REQUIRE(device->set_constants(device->self, compute_constants,
                                      sizeof(compute_constants)) == RESONATE_OK);
        REQUIRE(device->dispatch(device->self, 2U, 1U, 1U) == RESONATE_OK);
        ResonateRenderPassDesc pass = {};
        pass.struct_size = sizeof(pass);
        pass.color.value = 0U; /* the canvas */
        pass.clear_color = ResonateVec4{0.0F, 0.0F, 0.0F, 1.0F};
        pass.samples = samples;
        REQUIRE(device->begin_pass(device->self, &pass) == RESONATE_OK);
        REQUIRE(device->bind_pipeline(device->self, probes[samples == 1U ? 0U : 1U]) ==
                RESONATE_OK);
        /* The scene push's shape: model (64 B), tint (16 B), slots (16 B),
           slots2 (16 B), texture_slot (4 B). The fragment reads the frame
           slot and the texture slot. */
        std::uint32_t constants[29] = {};
        constants[20] = frame_slot;
        constants[28] = 0U; /* texture slot 0 */
        REQUIRE(device->set_constants(device->self, constants, sizeof(constants)) == RESONATE_OK);
        REQUIRE(device->draw(device->self, 3U, 1U) == RESONATE_OK);
        REQUIRE(device->end_pass(device->self) == RESONATE_OK);
        REQUIRE(device->present(device->self) == RESONATE_OK);
        REQUIRE(device->wait_idle(device->self) == RESONATE_OK);
    };
    const auto read_red = [&]
    {
        std::vector<std::uint8_t> pixels(64U * 64U * 4U);
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        REQUIRE(device->readback_canvas(device->self, pixels.data(),
                                        static_cast<std::uint64_t>(pixels.size()), &width,
                                        &height) == RESONATE_OK);
        return pixels[0];
    };
    std::uint32_t frame_runs = 0U;

    /* The renderer's structure: the scene pass renders into an explicit HDR
       image (the 4x resolve target), then a 1x post pass samples that image
       and writes the canvas. */
    ResonateRenderImageDesc hdr_desc = {};
    hdr_desc.struct_size = sizeof(hdr_desc);
    hdr_desc.format = RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM;
    hdr_desc.width = 64U;
    hdr_desc.height = 64U;
    hdr_desc.usage = RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED;
    ResonateRenderImage hdr = {};
    REQUIRE(device->create_image(device->self, &hdr_desc, &hdr) == RESONATE_OK);

    std::vector<std::uint8_t> copy_spirv;
    read_file(shader_directory + "/probe.probeCopy.spv", copy_spirv);
    ResonateRenderShader copy_shader = {};
    REQUIRE(device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT,
                                  copy_spirv.data(), copy_spirv.size(),
                                  &copy_shader) == RESONATE_OK);
    ResonateRenderGraphicsPipelineDesc copy_desc = {};
    copy_desc.struct_size = sizeof(copy_desc);
    copy_desc.vertex_shader = vertex;
    copy_desc.fragment_shader = copy_shader;
    copy_desc.color_format = RESONATE_RENDER_FORMAT_UNKNOWN; /* the canvas */
    ResonateRenderPipeline copy_pipeline = {};
    REQUIRE(device->create_pipeline(device->self, &copy_desc, &copy_pipeline) == RESONATE_OK);
    REQUIRE(device->bind_image_slot(device->self, 1U, hdr, 0U) == RESONATE_OK);

    const auto hdr_frame = [&](std::uint32_t samples)
    {
        REQUIRE(device->begin_frame(device->self) == RESONATE_OK);
        /* The previous frame left the HDR as a shader read; the scene pass
           needs it back as an attachment. */
        ResonateRenderBarrierDesc to_write = {};
        to_write.struct_size = sizeof(to_write);
        to_write.image = hdr;
        to_write.before_stages = RESONATE_RENDER_STAGE_FRAGMENT;
        to_write.after_stages = RESONATE_RENDER_STAGE_COLOR_OUTPUT;
        to_write.before_access = RESONATE_RENDER_ACCESS_SHADER_READ;
        to_write.after_access = RESONATE_RENDER_ACCESS_COLOR_WRITE;
        to_write.before_layout = RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY;
        to_write.after_layout = RESONATE_RENDER_LAYOUT_COLOR_ATTACHMENT;
        if (frame_runs != 0U)
        {
            REQUIRE(device->barrier(device->self, &to_write) == RESONATE_OK);
        }
        ++frame_runs;
        REQUIRE(device->update_buffer(device->self, frame_buffer, 0U, frame_bytes.data(),
                                      static_cast<std::uint64_t>(frame_bytes.size())) ==
                RESONATE_OK);
        ResonateRenderPassDesc pass = {};
        pass.struct_size = sizeof(pass);
        pass.color = hdr;
        pass.clear_color = ResonateVec4{0.0F, 0.0F, 0.0F, 1.0F};
        pass.samples = samples;
        REQUIRE(device->begin_pass(device->self, &pass) == RESONATE_OK);
        REQUIRE(device->bind_pipeline(device->self, probes[samples == 1U ? 0U : 1U]) ==
                RESONATE_OK);
        std::uint32_t constants[29] = {};
        constants[20] = frame_slot;
        constants[28] = 0U;
        REQUIRE(device->set_constants(device->self, constants, sizeof(constants)) == RESONATE_OK);
        REQUIRE(device->draw(device->self, 3U, 1U) == RESONATE_OK);
        REQUIRE(device->end_pass(device->self) == RESONATE_OK);
        /* The graph would emit this between the passes: the resolved target
           leaves the attachment layout and becomes a shader read. */
        ResonateRenderBarrierDesc to_read = {};
        to_read.struct_size = sizeof(to_read);
        to_read.image = hdr;
        to_read.before_stages = RESONATE_RENDER_STAGE_COLOR_OUTPUT;
        to_read.after_stages = RESONATE_RENDER_STAGE_FRAGMENT;
        to_read.before_access = RESONATE_RENDER_ACCESS_COLOR_WRITE;
        to_read.after_access = RESONATE_RENDER_ACCESS_SHADER_READ;
        to_read.before_layout = RESONATE_RENDER_LAYOUT_COLOR_ATTACHMENT;
        to_read.after_layout = RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY;
        REQUIRE(device->barrier(device->self, &to_read) == RESONATE_OK);
        /* The post stand-in: the canvas samples the resolved target. */
        ResonateRenderPassDesc post = {};
        post.struct_size = sizeof(post);
        post.color.value = 0U;
        post.clear_color = ResonateVec4{0.0F, 0.0F, 0.0F, 1.0F};
        REQUIRE(device->begin_pass(device->self, &post) == RESONATE_OK);
        REQUIRE(device->bind_pipeline(device->self, copy_pipeline) == RESONATE_OK);
        std::uint32_t post_constants[29] = {};
        post_constants[28] = 1U; /* the resolved HDR sits at table slot 1 */
        REQUIRE(device->set_constants(device->self, post_constants,
                                      sizeof(post_constants)) == RESONATE_OK);
        REQUIRE(device->draw(device->self, 3U, 1U) == RESONATE_OK);
        REQUIRE(device->end_pass(device->self) == RESONATE_OK);
        REQUIRE(device->present(device->self) == RESONATE_OK);
        REQUIRE(device->wait_idle(device->self) == RESONATE_OK);
    };

    hdr_frame(1U);
    hdr_frame(1U);
    const std::uint8_t at_1x = read_red();
    CHECK(at_1x > 200U); /* the white texel through the table and the copy */

    hdr_frame(4U);
    hdr_frame(4U);
    const std::uint8_t at_4x = read_red();
    INFO("probe texel at 1x=" << static_cast<int>(at_1x) << " at 4x=" << static_cast<int>(at_4x));
    CHECK(at_4x == at_1x);

    host->detachAll();
}
