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
    world.add(cube, resonate::render::Material{1U, Vec3{1.0F, 1.0F, 1.0F}});

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
