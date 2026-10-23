#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/module/host.hpp>
#include <resonate/render/device.h>
#include <resonate/render/offscreen.hpp>
#include <resonate/renderer/renderer.h>
#include <resonate/text/font.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/layout.h>

#include "paths.h"

namespace
{

const char* const kGlyphDocument = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="letter" text="A" font_size="48" color="0 0 0 0"
           text_color="1 0 1 1" width="120" height="80"/>
  </Screen>
</document>)xml";

} // namespace

/* Text is the one UI path whose failure is invisible without a device: the
   display list can be perfect while the atlas texture samples nothing. This
   renders a magenta glyph into the offscreen target and reads it back. */
TEST_CASE("offscreen: a UI Label draws its glyphs from the font atlas",
          "[render][offscreen][text]")
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

    std::string font_error;
    const auto font = resonate::text::Font::loadFile(
        std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/ResonateSansSC-Subset.ttf",
        font_error);
    REQUIRE(font != nullptr);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));

    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(
        kGlyphDocument, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    const resonate::ecs::Entity canvas = resonate::ui::instantiate(world, *document, nullptr, {font});
    REQUIRE(canvas.valid());
    REQUIRE(resonate::ui::layout(world, canvas, 320.0F, 240.0F));

    resonate::renderer::RenderSystem render_system;
    render_system.setRegistry(host->capabilities());
    for (int frame = 0; frame < 5; ++frame)
    {
        render_system.run(world);
    }
    render_system.waitIdle();

    std::vector<std::uint8_t> pixels(320U * 240U * 4U);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const ResonateStatus status = device->readback_canvas(
        device->self, pixels.data(), static_cast<std::uint64_t>(pixels.size()), &width, &height);
    if (status != RESONATE_OK)
    {
        render_system.release();
        SKIP("no Vulkan offscreen device on this machine");
    }
    REQUIRE(width == 320U);
    REQUIRE(height == 240U);

    std::uint32_t ink = 0;
    for (std::size_t index = 0; index + 3U < pixels.size(); index += 4U)
    {
        if (pixels[index] > 180U && pixels[index + 1U] < 80U && pixels[index + 2U] > 180U)
        {
            ++ink;
        }
    }
    INFO("magenta glyph pixels=" << ink);
    CHECK(ink > 20U);

    render_system.release();
    host->detachAll();
}
