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

#include "paths.h"
#include "resonate.render/components.gen.h"

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

    const auto camera = world.create();
    world.add(camera, resonate::hierarchy::WorldTransform{translation(0.0F, 2.5F, -7.0F)});
    world.add(camera, resonate::render::Camera{});

    const auto cube = world.create();
    world.add(cube, resonate::hierarchy::WorldTransform{translation(0.0F, 2.5F, 0.0F)});
    world.add(cube, resonate::render::MeshInstance{1U, Vec3{1.0F, 1.0F, 1.0F}});
    world.add(cube, resonate::render::Material{1U, Vec3{1.0F, 1.0F, 1.0F}});

    const auto sun = world.create();
    world.add(sun, resonate::render::DirectionalLight{Vec3{0.0F, 0.0F, 1.0F},
                                                      Vec3{1.0F, 1.0F, 1.0F}, 1.0F});

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
    CHECK(lit > 400U);

    render_system.release();
    host->detachAll();
}
