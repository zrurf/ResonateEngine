#include <catch2/catch_all.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/module/host.hpp>
#include <resonate/render/device.h>
#include <resonate/render/offscreen.hpp>
#include <resonate/text/font.h>
#include <resonate/ui_tgfx.h>

#include "paths.h"

namespace
{

/* One offscreen device, the tgfx UI layer over it, and the pixels it drew:
   every case here asserts what the wrapper rasterizes, not what the engine
   asked for. */
struct TgfxFixture
{
    std::unique_ptr<resonate::ModuleHost> host;
    ResonateRenderDevice* device = nullptr;
    ResonateTgfxUi ui = nullptr;
    std::string font_path;
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    TgfxFixture(std::uint32_t target_width, std::uint32_t target_height)
        : width(target_width), height(target_height)
    {
        const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
        INFO("the plugin targets are a build dependency of this test target");
        REQUIRE_FALSE(plugin_directory.empty());

        host = resonate::ModuleHost::create(resonate::systemAllocator());
        REQUIRE(host != nullptr);
        REQUIRE(resonate::render::publishOffscreen(host->capabilities(), width, height) ==
                RESONATE_OK);
        REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
        REQUIRE(host->resolve() == RESONATE_OK);
        REQUIRE(host->attachAll() == RESONATE_OK);

        device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
            host->capabilities(),
            resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(), 1U, nullptr));
        REQUIRE(device != nullptr);
        REQUIRE(device->native_handles != nullptr);
        REQUIRE(device->attach_offscreen(device->self, width, height) == RESONATE_OK);
        ResonateRenderNativeHandles handles = {};
        REQUIRE(device->native_handles(device->self, &handles) == RESONATE_OK);
        REQUIRE(handles.instance != 0U);
        REQUIRE(handles.device != 0U);

        font_path = std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/NotoSansSC-Regular.ttf";
        const std::vector<const char*> font_paths{font_path.c_str()};
        REQUIRE(resonate_tgfx_ui_create(reinterpret_cast<void*>(handles.instance),
                                        reinterpret_cast<void*>(handles.physical_device),
                                        reinterpret_cast<void*>(handles.device),
                                        reinterpret_cast<void*>(handles.queue),
                                        handles.queue_family, font_paths.data(), 1U, &ui) == 0);
        REQUIRE(ui != nullptr);
        pixels.assign(static_cast<std::size_t>(width) * height * 4U, 0U);
    }

    ~TgfxFixture()
    {
        if (ui != nullptr)
        {
            resonate_tgfx_ui_destroy(ui);
        }
        if (host != nullptr)
        {
            host->detachAll();
        }
    }

    TgfxFixture(const TgfxFixture&) = delete;
    TgfxFixture& operator=(const TgfxFixture&) = delete;

    /* Draws one layer with `rects` and reads it back. */
    void draw(const std::vector<ResonateTgfxRect>& rects)
    {
        REQUIRE(resonate_tgfx_ui_draw(ui, width, height, rects.data(),
                                      static_cast<std::uint32_t>(rects.size()), nullptr, 0U,
                                      nullptr, 0U) == 0);
        REQUIRE(resonate_tgfx_ui_read_pixels(ui, pixels.data(), pixels.size()) == 0);
    }

    /* The premultiplied RGBA of one pixel. */
    const std::uint8_t* at(std::uint32_t x, std::uint32_t y) const
    {
        return pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

} // namespace

TEST_CASE("tgfx shares the render device and draws shaped CJK text", "[render][tgfx]")
{
    TgfxFixture fixture(256U, 128U);

    /* The engine shapes; the wrapper only draws the glyph origins. */
    std::string font_error;
    const auto font = resonate::text::Font::loadFile(fixture.font_path, font_error);
    REQUIRE(font != nullptr);
    std::vector<resonate::text::ShapedGlyph> shaped;
    REQUIRE(resonate::text::shape(*font, "\xE5\x8A\xA8\xE4\xBD\x9C\xE6\xBC\x94\xE7\xA4\xBA", 24.0F,
                                  400.0F, shaped, font_error));
    REQUIRE(shaped.size() == 4U);
    std::vector<ResonateTgfxGlyph> glyphs;
    glyphs.reserve(shaped.size());
    float pen = 24.0F;
    for (const resonate::text::ShapedGlyph& glyph : shaped)
    {
        glyphs.push_back(ResonateTgfxGlyph{pen, 40.0F, glyph.glyph, 0U});
        pen += glyph.x_advance;
    }

    ResonateTgfxRect background{};
    background.width = 256.0F;
    background.height = 128.0F;
    background.a = 1.0F;
    ResonateTgfxText run{24.0F, 1.0F, 1.0F, 1.0F, 1.0F, glyphs.data(),
                         static_cast<std::uint32_t>(glyphs.size())};
    REQUIRE(resonate_tgfx_ui_draw(fixture.ui, 256U, 128U, &background, 1U, &run, 1U, nullptr,
                                  0U) == 0);
    REQUIRE(resonate_tgfx_ui_read_pixels(fixture.ui, fixture.pixels.data(),
                                         fixture.pixels.size()) == 0);

    std::uint32_t ink = 0U;
    for (std::size_t index = 0; index + 3U < fixture.pixels.size(); index += 4U)
    {
        ink += fixture.pixels[index] > 200U && fixture.pixels[index + 1U] > 200U &&
                       fixture.pixels[index + 2U] > 200U
                   ? 1U
                   : 0U;
    }
    INFO("white pixels=" << ink);
    CHECK(ink > 40U);
}

TEST_CASE("tgfx rounds a fill's corners and keeps a border to its ring", "[render][tgfx]")
{
    TgfxFixture fixture(128U, 128U);

    /* A 96 px box at (16, 16) with a 24 px radius: the corner leaves the panel
       showing, the arc's inside is filled. */
    {
        ResonateTgfxRect box{};
        box.x = 16.0F;
        box.y = 16.0F;
        box.width = 96.0F;
        box.height = 96.0F;
        box.radius = 24.0F;
        box.r = 1.0F;
        box.g = 1.0F;
        box.b = 1.0F;
        box.a = 1.0F;
        fixture.draw({box});
        CHECK(fixture.at(16U, 16U)[3] < 8U);
        CHECK(fixture.at(40U, 17U)[3] > 240U);
        CHECK(fixture.at(64U, 64U)[3] > 250U);
    }

    /* The same box as a 4 px ring: only the edge lights up. */
    {
        ResonateTgfxRect ring{};
        ring.x = 16.0F;
        ring.y = 16.0F;
        ring.width = 96.0F;
        ring.height = 96.0F;
        ring.radius = 24.0F;
        ring.border = 4.0F;
        ring.r = 1.0F;
        ring.g = 1.0F;
        ring.b = 1.0F;
        ring.a = 1.0F;
        fixture.draw({ring});
        CHECK(fixture.at(16U, 16U)[3] < 8U);
        CHECK(fixture.at(64U, 18U)[3] > 240U);
        CHECK(fixture.at(64U, 15U)[3] < 8U);
        CHECK(fixture.at(64U, 24U)[3] < 8U);
        CHECK(fixture.at(64U, 64U)[3] < 8U);
    }
}
