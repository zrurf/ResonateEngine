#include <catch2/catch_all.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <tgfx/core/Canvas.h>
#include <tgfx/core/Color.h>
#include <tgfx/core/Font.h>
#include <tgfx/core/ImageInfo.h>
#include <tgfx/core/Paint.h>
#include <tgfx/core/Surface.h>
#include <tgfx/core/Typeface.h>
#include <tgfx/gpu/vulkan/VulkanDevice.h>

#include <resonate/core/allocator.h>
#include <resonate/module/host.hpp>
#include <resonate/render/device.h>
#include <resonate/render/offscreen.hpp>

#include "paths.h"

TEST_CASE("tgfx shares the render device and draws CJK text", "[render][tgfx]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    INFO("the plugin targets are a build dependency of this test target");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 256U, 128U) == RESONATE_OK);
    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    auto* device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
        host->capabilities(), resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(),
        1U, nullptr));
    REQUIRE(device != nullptr);
    REQUIRE(device->native_handles != nullptr);
    /* The module creates the Vulkan objects on the first target attach. */
    REQUIRE(device->attach_offscreen(device->self, 256U, 128U) == RESONATE_OK);
    ResonateRenderNativeHandles handles = {};
    REQUIRE(device->native_handles(device->self, &handles) == RESONATE_OK);
    REQUIRE(handles.instance != 0U);
    REQUIRE(handles.physical_device != 0U);
    REQUIRE(handles.device != 0U);
    REQUIRE(handles.queue != 0U);

    auto tgfx_device = tgfx::VulkanDevice::MakeFrom(
        reinterpret_cast<void*>(handles.instance),
        reinterpret_cast<void*>(handles.physical_device),
        reinterpret_cast<void*>(handles.device), reinterpret_cast<void*>(handles.queue),
        handles.queue_family);
    REQUIRE(tgfx_device != nullptr);
    auto* context = tgfx_device->lockContext();
    REQUIRE(context != nullptr);

    auto surface = tgfx::Surface::Make(context, 256, 128);
    REQUIRE(surface != nullptr);
    auto canvas = surface->getCanvas();
    canvas->clear(tgfx::Color::White());
    auto typeface = tgfx::Typeface::MakeFromPath(std::string(RESONATE_SOURCE_DIR) +
                                                 "/content/ui/fonts/ResonateSansSC-Subset.ttf");
    REQUIRE(typeface != nullptr);
    tgfx::Font font(typeface, 24.0F);
    tgfx::Paint paint;
    paint.setColor(tgfx::Color::Black());
    canvas->drawSimpleText("\xE5\x8A\xA8\xE4\xBD\x9C\xE6\xBC\x94\xE7\xA4\xBA", 12.0F, 48.0F,
                           font, paint);

    const tgfx::ImageInfo info = tgfx::ImageInfo::Make(
        256, 128, tgfx::ColorType::RGBA_8888, tgfx::AlphaType::Premultiplied);
    std::vector<std::uint8_t> pixels(256U * 128U * 4U);
    REQUIRE(surface->readPixels(info, pixels.data()));
    tgfx_device->unlock();

    std::uint32_t ink = 0;
    for (std::size_t index = 0; index + 3U < pixels.size(); index += 4U)
    {
        ink += pixels[index] < 64U && pixels[index + 1U] < 64U && pixels[index + 2U] < 64U ? 1U
                                                                                           : 0U;
    }
    INFO("black pixels=" << ink);
    /* Four CJK glyphs at 24px cover far more than this; a blank or upside-down
       surface would not. */
    CHECK(ink > 100U);

    /* tgfx's objects hold the shared VkDevice; release them before the module
       that owns the device detaches. */
    surface.reset();
    typeface.reset();
    tgfx_device.reset();
    host->detachAll();
}
