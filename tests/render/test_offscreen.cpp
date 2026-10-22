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
