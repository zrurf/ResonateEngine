#include <catch2/catch_all.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/module/host.hpp>
#include <resonate/render/device.h>
#include <resonate/render/offscreen.hpp>
#include <resonate/ui_tgfx.h>

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
    REQUIRE(device->attach_offscreen(device->self, 256U, 128U) == RESONATE_OK);
    ResonateRenderNativeHandles handles = {};
    REQUIRE(device->native_handles(device->self, &handles) == RESONATE_OK);
    REQUIRE(handles.instance != 0U);
    REQUIRE(handles.device != 0U);

    ResonateTgfxUi ui = nullptr;
    const std::string font =
        std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/ResonateSansSC-Subset.ttf";
    REQUIRE(resonate_tgfx_ui_create(reinterpret_cast<void*>(handles.instance),
                                    reinterpret_cast<void*>(handles.physical_device),
                                    reinterpret_cast<void*>(handles.device),
                                    reinterpret_cast<void*>(handles.queue), handles.queue_family,
                                    font.c_str(), &ui) == 0);
    REQUIRE(ui != nullptr);

    ResonateTgfxRect background{0.0F, 0.0F, 256.0F, 128.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const char* const text = "\xE5\x8A\xA8\xE4\xBD\x9C\xE6\xBC\x94\xE7\xA4\xBA";
    ResonateTgfxText run{12.0F, 24.0F, 200.0F, 32.0F, 24.0F, 1.0F, 1.0F, 1.0F, 1.0F, 400U, 0U,
                         text, 12U};
    REQUIRE(resonate_tgfx_ui_draw(ui, 256U, 128U, &background, 1U, &run, 1U) == 0);

    std::vector<std::uint8_t> pixels(256U * 128U * 4U);
    REQUIRE(resonate_tgfx_ui_read_pixels(ui, pixels.data(), pixels.size()) == 0);
    std::uint32_t ink = 0;
    for (std::size_t index = 0; index + 3U < pixels.size(); index += 4U)
    {
        ink += pixels[index] > 200U && pixels[index + 1U] > 200U && pixels[index + 2U] > 200U
                   ? 1U
                   : 0U;
    }
    INFO("white pixels=" << ink);
    CHECK(ink > 40U);

    resonate_tgfx_ui_destroy(ui);
    host->detachAll();
}
