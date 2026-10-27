#include <catch2/catch_all.hpp>

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

const char* const kUiDocument = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="title" text="动作演示" font_size="32" width="300" height="60"
           color="0.05 0.06 0.09 1" text_color="1 1 1 1"/>
  </Screen>
</document>)xml";

const char* const kImageDocument = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Picture key="logo" name="Logo" texture="16" x="10" y="10" width="48" height="48"
             position="Absolute"/>
  </Screen>
</document>)xml";

} // namespace

TEST_CASE("offscreen: the renderer draws the UI through tgfx", "[render][tgfx]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
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

    const std::string font_path =
        std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/NotoSansSC-Regular.ttf";
    std::string font_error;
    const auto font = resonate::text::Font::loadFile(font_path, font_error);
    REQUIRE(font != nullptr);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));

    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kUiDocument, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    const resonate::ecs::Entity canvas =
        resonate::ui::instantiate(world, *document, nullptr, {font});
    REQUIRE(canvas.valid());
    REQUIRE(resonate::ui::layout(world, canvas, 320.0F, 240.0F));

    resonate::renderer::RenderSystem render_system;
    render_system.setRegistry(host->capabilities());
    render_system.setUiFontPaths({font_path});
    for (int frame = 0; frame < 8; ++frame)
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
        host->detachAll();
        SKIP("no Vulkan offscreen device on this machine");
    }
    REQUIRE(width == 320U);
    REQUIRE(height == 240U);

    std::uint32_t white = 0;
    for (std::uint32_t y = 0; y < 60U; ++y)
    {
        for (std::uint32_t x = 0; x < 300U; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4U;
            white += pixels[index] > 200U && pixels[index + 1U] > 200U &&
                             pixels[index + 2U] > 200U
                         ? 1U
                         : 0U;
        }
    }
    INFO("white text pixels=" << white);
    /* The blend pass composites the UI after FXAA, so the bold sample keeps
       its full stroke coverage. */
    CHECK(white > 300U);

    render_system.release();
    host->detachAll();
}

TEST_CASE("offscreen: the renderer draws a registered Picture image", "[render][tgfx]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
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

    const std::string font_path =
        std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/NotoSansSC-Regular.ttf";
    std::string font_error;
    const auto font = resonate::text::Font::loadFile(font_path, font_error);
    REQUIRE(font != nullptr);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));

    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kImageDocument, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    const resonate::ecs::Entity canvas =
        resonate::ui::instantiate(world, *document, nullptr, {font});
    REQUIRE(canvas.valid());
    REQUIRE(resonate::ui::layout(world, canvas, 320.0F, 240.0F));

    const std::string image_path =
        std::string(RESONATE_SOURCE_DIR) + "/content/ui/images/logo.png";
    resonate::renderer::RenderSystem render_system;
    render_system.setRegistry(host->capabilities());
    render_system.setUiFontPaths({font_path});
    render_system.setUiImageTexture(16U, image_path);
    /* The render thread may coalesce back-to-back publishes, and frame one
       draws the atlas fallback (the UI target is created only past the first
       present for the Mali quirk), so drain between frames: the readback must
       see a tgfx frame. */
    for (int frame = 0; frame < 8; ++frame)
    {
        render_system.run(world);
        render_system.waitIdle();
    }
    render_system.waitIdle();

    std::vector<std::uint8_t> pixels(320U * 240U * 4U);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const ResonateStatus status = device->readback_canvas(
        device->self, pixels.data(), static_cast<std::uint64_t>(pixels.size()), &width, &height);
    if (std::getenv("RESONATE_DUMP_TEST_CANVAS") != nullptr)
    {
        std::FILE* out = std::fopen(std::getenv("RESONATE_DUMP_TEST_CANVAS"), "wb");
        if (out != nullptr)
        {
            const std::uint32_t stride = (width * 3U + 3U) & ~3U;
            unsigned char header[54] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0,
                                        0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0};
            const auto put32 = [&header](std::size_t at, std::uint32_t v) {
                for (int b = 0; b < 4; ++b)
                {
                    header[at + static_cast<std::size_t>(b)] =
                        static_cast<unsigned char>((v >> (8U * b)) & 0xFFU);
                }
            };
            put32(2U, 54U + stride * height);
            put32(18U, width);
            put32(22U, height);
            header[26] = 1U;
            header[28] = 24U;
            put32(34U, stride * height);
            std::fwrite(header, 1U, sizeof(header), out);
            std::vector<unsigned char> row(stride, 0U);
            for (std::uint32_t y = height; y-- > 0U;)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const std::size_t s = (static_cast<std::size_t>(y) * width + x) * 4U;
                    row[x * 3U] = pixels[s + 2U];
                    row[x * 3U + 1U] = pixels[s + 1U];
                    row[x * 3U + 2U] = pixels[s];
                }
                std::fwrite(row.data(), 1U, stride, out);
            }
            std::fclose(out);
        }
    }
    if (status != RESONATE_OK)
    {
        render_system.release();
        host->detachAll();
        SKIP("no Vulkan offscreen device on this machine");
    }
    REQUIRE(width == 320U);
    REQUIRE(height == 240U);

    /* The 64px sample: white 8px border, magenta left half, cyan right half.
       Inside the 48px rect the mid rows read magenta then cyan. */
    const auto pixelAt = [&pixels, width](std::uint32_t x, std::uint32_t y)
    {
        const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4U;
        return (static_cast<std::uint32_t>(pixels[index]) << 16U) |
               (static_cast<std::uint32_t>(pixels[index + 1U]) << 8U) |
               static_cast<std::uint32_t>(pixels[index + 2U]);
    };
    INFO("left=" << pixelAt(22U, 34U) << " right=" << pixelAt(38U, 34U));
    CHECK(pixelAt(22U, 34U) == 0xFF00FFU);
    CHECK(pixelAt(38U, 34U) == 0x00FFFFU);
    /* The border frame stays white against the dark scene. */
    CHECK(pixelAt(13U, 34U) == 0xFFFFFFU);

    render_system.release();
    host->detachAll();
}
