#include <catch2/catch_all.hpp>

#include <cstdio>
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

TEST_CASE("offscreen: the widget gallery renders", "[render][gallery]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    constexpr std::uint32_t kWidth = 1000U;
    constexpr std::uint32_t kHeight = 1120U;
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
    REQUIRE(readSourceFile("content/ui/gallery.rui", source));
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
    const resonate::ecs::Entity canvas =
        resonate::ui::instantiate(world, *document, nullptr, runtime);
    REQUIRE(canvas.valid());
    REQUIRE(resonate::ui::layout(world, canvas, static_cast<float>(kWidth),
                                 static_cast<float>(kHeight)));

    resonate::renderer::RenderSystem render_system;
    render_system.setRegistry(host->capabilities());
    render_system.setUiFontPaths(std::move(font_paths));
    for (int frame = 0; frame < 8; ++frame)
    {
        render_system.run(world);
        render_system.waitIdle();
    }

    /* A click focuses the empty field. Its caret is the only bright pixel in a
       box that holds no text, so the readback can prove where it landed: the
       caret used to be drawn along the box top for any focused widget, which
       put a column on checkboxes and off the text baseline. */
    const resonate::ecs::Entity empty_field =
        resonate::ui::findByKey(world, canvas, "empty-field");
    REQUIRE(empty_field.valid());
    const auto* field_rect = world.get<resonate::ui::ComputedRect>(empty_field);
    REQUIRE(field_rect != nullptr);
    const float field_x = field_rect->x;
    const float field_y = field_rect->y;
    const float field_w = field_rect->width;
    const float field_h = field_rect->height;
    REQUIRE(resonate::ui::updateInput(world, canvas, field_x + field_w * 0.5F,
                                      field_y + field_h * 0.5F, true));
    REQUIRE(resonate::ui::updateInput(world, canvas, field_x + field_w * 0.5F,
                                      field_y + field_h * 0.5F, false));
    render_system.run(world);
    render_system.waitIdle();

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

    if (const char* dump = std::getenv("RESONATE_DUMP_GALLERY"); dump != nullptr)
    {
        writeBmp(dump, pixels, width, height);
    }

    /* The gallery's palette shows on screen: the backdrop, panel greys, the
       green HP-like bar and the accent slider fill all have to be there. The
       scene behind is the demo: the panel tones are distinct enough to count
       flat regions. */
    std::uint32_t panel_pixels = 0U;
    std::uint32_t accent_pixels = 0U;
    std::uint32_t bright_text = 0U;
    for (std::size_t index = 0U; index < pixels.size(); index += 4U)
    {
        const int red = pixels[index];
        const int green = pixels[index + 1U];
        const int blue = pixels[index + 2U];
        panel_pixels += red > 28 && red < 48 && green > 30 && green < 50 && blue > 36 &&
                                blue < 56
                            ? 1U
                            : 0U;
        accent_pixels += red > 190 && green > 120 && green < 175 && blue < 80 ? 1U : 0U;
        bright_text += red > 190 && green > 190 && blue > 190 ? 1U : 0U;
    }
    INFO("panel=" << panel_pixels << " accent=" << accent_pixels << " text=" << bright_text);
    CHECK(panel_pixels > 20000U);
    CHECK(accent_pixels > 2000U);
    CHECK(bright_text > 500U);

    /* The caret inside the empty field: a bright column whose top and bottom
       both clear the box edge by the same margin, i.e. centred, not clipped to
       one end. */
    int caret_top = -1;
    int caret_bottom = -1;
    int caret_columns = 0;
    const auto left = static_cast<int>(field_x) + 1;
    const auto right = static_cast<int>(field_x + field_w) - 1;
    const auto top = static_cast<int>(field_y) + 1;
    const auto bottom = static_cast<int>(field_y + field_h) - 1;
    for (int y = top; y < bottom; ++y)
    {
        bool bright = false;
        for (int x = left; x < right; ++x)
        {
            const std::size_t at = (static_cast<std::size_t>(y) * width +
                                    static_cast<std::size_t>(x)) *
                                   4U;
            if (pixels[at] > 150U && pixels[at + 1U] > 150U && pixels[at + 2U] > 150U)
            {
                bright = true;
            }
        }
        if (bright)
        {
            if (caret_top < 0)
            {
                caret_top = y;
            }
            caret_bottom = y;
            ++caret_columns;
        }
    }
    INFO("caret rows " << caret_top << ".." << caret_bottom << " of " << top << ".." << bottom);
    REQUIRE(caret_columns > 0);
    CHECK(caret_top - top >= 2);
    CHECK(bottom - 1 - caret_bottom >= 2);

    render_system.release();
    host->detachAll();
}
