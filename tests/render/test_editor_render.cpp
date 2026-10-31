#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
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
#include <resonate/ui/display_list.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/hit_test.h>
#include <resonate/ui/icon_registry.h>
#include <resonate/ui/image_registry.h>
#include <resonate/ui/input.h>
#include <resonate/ui/layout.h>
#include <resonate/ui/style.h>
#include <resonate/ui/text_runtime.h>

#include "resonate.ui/components.gen.h"

#include "paths.h"

namespace
{

/* The shipped gallery, stylesheets and fonts, resolved from the source tree. */
bool readSourceFile(const std::string& relative, std::string& out_text)
{
    std::ifstream in(std::string(RESONATE_SOURCE_DIR) + "/" + relative, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }
    out_text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void writeBmp(const char* path, const std::vector<std::uint8_t>& pixels, std::uint32_t width,
              std::uint32_t height)
{
    std::FILE* out = std::fopen(path, "wb");
    if (out == nullptr)
    {
        return;
    }
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

} // namespace

TEST_CASE("offscreen: the editor shell renders", "[render][editor]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    constexpr std::uint32_t kWidth = 1280U;
    constexpr std::uint32_t kHeight = 800U;
    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), kWidth, kHeight) ==
            RESONATE_OK);
    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    auto* device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
        host->capabilities(), resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(),
        1U, nullptr));
    REQUIRE(device != nullptr);

    std::vector<std::shared_ptr<const resonate::text::Font>> fonts;
    std::vector<std::string> font_paths;
    for (const char* name : {"NotoSansSC-Regular.ttf", "NotoSansSC-Medium.ttf",
                             "NotoSansSC-Bold.ttf"})
    {
        const std::string path = std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/" + name;
        std::string error;
        auto font = resonate::text::Font::loadFile(path, error);
        REQUIRE(font != nullptr);
        font_paths.push_back(path);
        fonts.push_back(std::move(font));
    }

    std::string source;
    REQUIRE(readSourceFile("content/ui/editor.rui", source));
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(source, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    REQUIRE(resonate::ui::loadStylesheets(
        *document, nullptr,
        [](void*, std::string_view path, std::string& out_text)
        { return readSourceFile(std::string(path), out_text); },
        diagnostics));

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));

    resonate::ui::TextRuntime runtime(fonts);
    /* The icon atlas registers like any shipped image; the map names the
       sub-rect each icon occupies, so both must agree on one texture id. */
    resonate::ui::ImageRegistry images;
    const std::uint32_t icon_atlas = images.registerImage("icons.png");
    resonate::ui::IconRegistry icons;
    {
        std::string map_text;
        REQUIRE(readSourceFile("content/ui/icons.icons", map_text));
        std::string icon_error;
        REQUIRE(icons.load(map_text, icon_error));
        icons.setTexture(icon_atlas);
    }
    const resonate::ecs::Entity canvas =
        resonate::ui::instantiate(world, *document, nullptr, runtime, nullptr, &images, &icons);
    REQUIRE(canvas.valid());
    REQUIRE(resonate::ui::layout(world, canvas, static_cast<float>(kWidth),
                                 static_cast<float>(kHeight)));

    resonate::renderer::RenderSystem render_system;
    render_system.setRegistry(host->capabilities());
    render_system.setUiFontPaths(std::move(font_paths));
    render_system.setUiImageTexture(
        icon_atlas, std::string(RESONATE_SOURCE_DIR) + "/content/ui/images/icons.png");
    for (int frame = 0; frame < 8; ++frame)
    {
        render_system.run(world);
        render_system.waitIdle();
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4U);
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    const ResonateStatus status = device->readback_canvas(
        device->self, pixels.data(), static_cast<std::uint64_t>(pixels.size()), &width, &height);
    if (status != RESONATE_OK)
    {
        render_system.release();
        host->detachAll();
        SKIP("no Vulkan offscreen device on this machine");
    }
    REQUIRE(width == kWidth);
    REQUIRE(height == kHeight);

    if (const char* dump = std::getenv("RESONATE_DUMP_EDITOR"); dump != nullptr)
    {
        writeBmp(dump, pixels, width, height);
    }

    /* The shell's surfaces are on screen: the panel grey of the panes, the
       header tier of the toolbar and pane headers, and the selected row's
       neutral fill. The viewport stays transparent over the scene. */
    std::uint32_t panel_pixels = 0U;
    std::uint32_t header_pixels = 0U;
    std::uint32_t selected_pixels = 0U;
    std::uint32_t bright_text = 0U;
    for (std::size_t index = 0U; index < pixels.size(); index += 4U)
    {
        const int red = pixels[index];
        const int green = pixels[index + 1U];
        const int blue = pixels[index + 2U];
        /* The render quantizes the linear tokens to the UI target's 8 bits:
           panel #242424 lands on 34. The header tier lands on 38 through the
           tgfx layer and on 42 through the vertex path, which quantizes before
           the composite encodes, so the band spans both. */
        panel_pixels += red >= 32 && red <= 36 && green >= 32 && green <= 36 && blue >= 32 &&
                                blue <= 36
                            ? 1U
                            : 0U;
        header_pixels += red >= 36 && red <= 43 && green >= 36 && green <= 43 && blue >= 36 &&
                                 blue <= 43
                             ? 1U
                             : 0U;
        selected_pixels += red > 78 && red < 90 && green > 78 && green < 90 && blue > 78 &&
                                   blue < 90
                               ? 1U
                               : 0U;
        bright_text += red > 220 && green > 220 && blue > 220 ? 1U : 0U;
    }
    INFO("panel=" << panel_pixels << " header=" << header_pixels
                  << " selected=" << selected_pixels << " text=" << bright_text);
    CHECK(panel_pixels > 100000U);
    CHECK(header_pixels > 20000U);
    CHECK(selected_pixels > 2000U);
    CHECK(bright_text > 300U);

    /* A toolbar icon samples the atlas and takes its class's tint: the strokes
       land between the header tone and the bright text tone. The vertex path
       draws no images, so a run with RESONATE_UI_NO_TGFX skips this. */
    if (std::getenv("RESONATE_UI_NO_TGFX") == nullptr)
    {
        const resonate::ecs::Entity icon =
            resonate::ui::findByKey(world, canvas, "tb-save-icon");
        REQUIRE(icon.valid());
        const auto* rect = world.get<resonate::ui::ComputedRect>(icon);
        REQUIRE(rect != nullptr);
        std::uint32_t strokes = 0U;
        for (int y = static_cast<int>(rect->y); y < static_cast<int>(rect->y + rect->height); ++y)
        {
            for (int x = static_cast<int>(rect->x); x < static_cast<int>(rect->x + rect->width);
                 ++x)
            {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4U;
                const int red = pixels[offset];
                strokes += red > 120 && red < 200 ? 1U : 0U;
            }
        }
        INFO("icon strokes=" << strokes);
        CHECK(strokes > 20U);
    }

    /* A coarse golden frame: the target box-filtered 4x4, so a layout or
       palette regression anywhere fails even where the bands above stay in
       range. RESONATE_UPDATE_BASELINE=1 rewrites the file; the tolerance
       absorbs the few pixels the smoothed fps readout moves. */
    {
        constexpr std::uint32_t kBlocks = 4U;
        const std::uint32_t golden_width = kWidth / kBlocks;
        const std::uint32_t golden_height = kHeight / kBlocks;
        std::vector<std::uint8_t> golden(static_cast<std::size_t>(golden_width) * golden_height *
                                         4U);
        for (std::uint32_t y = 0U; y < golden_height; ++y)
        {
            for (std::uint32_t x = 0U; x < golden_width; ++x)
            {
                std::uint32_t sums[4] = {};
                for (std::uint32_t dy = 0U; dy < kBlocks; ++dy)
                {
                    for (std::uint32_t dx = 0U; dx < kBlocks; ++dx)
                    {
                        const std::size_t offset = ((static_cast<std::size_t>(y * kBlocks + dy) *
                                                         kWidth) +
                                                    x * kBlocks + dx) *
                                                   4U;
                        for (std::uint32_t channel = 0U; channel < 4U; ++channel)
                        {
                            sums[channel] += pixels[offset + channel];
                        }
                    }
                }
                const std::size_t target =
                    (static_cast<std::size_t>(y) * golden_width + x) * 4U;
                for (std::uint32_t channel = 0U; channel < 4U; ++channel)
                {
                    golden[target + channel] =
                        static_cast<std::uint8_t>(sums[channel] / (kBlocks * kBlocks));
                }
            }
        }

        const std::string baseline =
            std::string(RESONATE_SOURCE_DIR) + "/tests/render/baselines/editor_shell.rgba";
        if (std::getenv("RESONATE_UPDATE_BASELINE") != nullptr)
        {
            std::ofstream out(baseline, std::ios::binary);
            REQUIRE(out.is_open());
            out.write(reinterpret_cast<const char*>(golden.data()),
                      static_cast<std::streamsize>(golden.size()));
        }
        else
        {
            std::ifstream in(baseline, std::ios::binary);
            INFO("baseline: " << baseline);
            REQUIRE(in.is_open());
            std::vector<std::uint8_t> stored((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
            REQUIRE(stored.size() == golden.size());
            std::size_t changed = 0U;
            std::uint64_t total = 0U;
            for (std::size_t index = 0U; index < golden.size(); index += 4U)
            {
                int delta = 0;
                for (std::uint32_t channel = 0U; channel < 4U; ++channel)
                {
                    delta = std::max(
                        delta, std::abs(static_cast<int>(golden[index + channel]) -
                                        static_cast<int>(stored[index + channel])));
                }
                changed += delta > 16 ? 1U : 0U;
                total += static_cast<std::uint64_t>(delta);
            }
            const std::size_t count = golden.size() / 4U;
            const double mean = static_cast<double>(total) / static_cast<double>(count);
            INFO("golden: " << changed << " of " << count << " pixels differ, mean " << mean);
            CHECK(changed * 100U < count * 2U);
            CHECK(mean < 3.0);

            /* The editor is a desktop target and nothing in its toolbar band is
               timing-dependent, so that band gets a strict check: a shifted
               button or a recooked icon moves most of it, which the whole-frame
               tolerance above is loose enough to miss. The small allowance is
               for GPU-to-GPU filtering differences, not for layout changes. */
            constexpr std::uint32_t kToolbarPixels = 40U;
            const std::uint32_t toolbar_rows = kToolbarPixels / kBlocks;
            std::size_t toolbar_changed = 0U;
            for (std::uint32_t y = 0U; y < toolbar_rows; ++y)
            {
                for (std::uint32_t x = 0U; x < golden_width; ++x)
                {
                    const std::size_t index =
                        (static_cast<std::size_t>(y) * golden_width + x) * 4U;
                    int delta = 0;
                    for (std::uint32_t channel = 0U; channel < 4U; ++channel)
                    {
                        delta = std::max(
                            delta, std::abs(static_cast<int>(golden[index + channel]) -
                                            static_cast<int>(stored[index + channel])));
                    }
                    toolbar_changed += delta > 16 ? 1U : 0U;
                }
            }
            const std::size_t toolbar_count =
                static_cast<std::size_t>(toolbar_rows) * golden_width;
            INFO("toolbar: " << toolbar_changed << " of " << toolbar_count << " blocks differ");
            CHECK(toolbar_changed * 100U < toolbar_count);
        }
    }

    /* Hiding a pane drops its paint — what the editor's toolbar toggles do —
       so the frame behind the UI shows where the panel grey was. */
    {
        const resonate::ecs::Entity scene = resonate::ui::findByKey(world, canvas, "scene-pane");
        REQUIRE(scene.valid());
        const auto* rect = world.get<resonate::ui::ComputedRect>(scene);
        REQUIRE(rect != nullptr);
        const auto x = static_cast<std::uint32_t>(rect->x + rect->width * 0.5F);
        const auto y = static_cast<std::uint32_t>(rect->y + 400.0F);
        const auto red_at = [&pixels, width](std::uint32_t px, std::uint32_t py)
        { return static_cast<int>(pixels[(static_cast<std::size_t>(py) * width + px) * 4U]); };
        INFO("scene tone before hiding=" << red_at(x, y));
        CHECK(red_at(x, y) >= 32);
        resonate::ui::setShown(world, scene, false);
        for (int frame = 0; frame < 4; ++frame)
        {
            render_system.run(world);
            render_system.waitIdle();
        }
        REQUIRE(device->readback_canvas(device->self, pixels.data(),
                                        static_cast<std::uint64_t>(pixels.size()), &width,
                                        &height) == RESONATE_OK);
        const int after = red_at(x, y);
        INFO("scene tone after hiding=" << after);
        CHECK_FALSE((after >= 32 && after <= 36));
        resonate::ui::setShown(world, scene, true);
    }

    /* The renderer owns the render thread and the device calls; it releases
       before the host detaches, or the teardown blocks. */
    render_system.release();
    host->detachAll();
    host.reset();
}
