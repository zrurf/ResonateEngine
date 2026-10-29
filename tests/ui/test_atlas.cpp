#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <resonate/text/font.h>

#include "../../packages/ui/src/glyph_run.h"

namespace
{

std::shared_ptr<const resonate::text::Font> loadFontFile(const char* name)
{
    std::string error;
    auto font = resonate::text::Font::loadFile(
        std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/" + name, error);
    REQUIRE(font != nullptr);
    return font;
}

std::shared_ptr<const resonate::text::Font> loadFont()
{
    return loadFontFile("NotoSansSC-Regular.ttf");
}

} // namespace

TEST_CASE("ui atlas: a full page grows and repacks its glyphs", "[ui][text][atlas]")
{
    resonate::ui::detail::TextAtlasBuilder atlas({loadFont()}, 32U);
    std::vector<resonate::ui::detail::GlyphQuad> quads;
    float advance = 0.0F;
    std::string error;
    REQUIRE(atlas.buildRun(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        "\xE7\x94\x9F\xE5\x91\xBD\xE5\x80\xBC\xE5\xBC\xB9\xE8\x8D\xAF\xE5\x8A\xA8\xE4\xBD\x9C"
        "\xE6\xBC\x94\xE7\xA4\xBA\xE6\x95\xB0\xE6\x8D\xAE",
        24.0F, 400.0F, 0.0F, 0U, false, false, quads, advance, error));
    CHECK(atlas.width() > 32U);
    CHECK(atlas.width() <= 2048U);
    CHECK(atlas.used());
    REQUIRE_FALSE(quads.empty());
    for (resonate::ui::detail::GlyphQuad& quad : quads)
    {
        REQUIRE(atlas.updateQuad(quad));
        CHECK(quad.u0 >= 0.0F);
        CHECK(quad.v0 >= 0.0F);
        CHECK(quad.u1 <= 1.0F);
        CHECK(quad.v1 <= 1.0F);
    }
}

TEST_CASE("ui atlas: a missing glyph falls through the font chain", "[ui][text][atlas]")
{
    /* A fixture face that carries ASCII only, so the chain has to reach the
       full face for the CJK glyph. */
    std::string error;
    const auto latin = resonate::text::Font::loadFile(
        std::string(RESONATE_SOURCE_DIR) + "/tests/data/NotoTestLatin-Regular.ttf", error);
    REQUIRE(latin != nullptr);
    const auto full = loadFont();
    CHECK(latin->glyphIndex(0x4F60U) == 0U);
    CHECK(full->glyphIndex(0x4F60U) != 0U);

    resonate::ui::detail::TextAtlasBuilder atlas({latin, full}, 128U);
    std::vector<resonate::ui::detail::GlyphQuad> quads;
    float advance = 0.0F;
    error.clear();
    REQUIRE(atlas.buildRun("A\xE4\xBD\xA0", 24.0F, 400.0F, 0.0F, 0U, false, false, quads,
                           advance, error));
    REQUIRE(quads.size() == 2U);
    CHECK((quads[0].key >> 56U) == 0U);
    CHECK((quads[1].key >> 56U) == 1U);
}

TEST_CASE("ui atlas: the medium weight takes its own face", "[ui][text][atlas]")
{
    const auto regular = loadFontFile("NotoSansSC-Regular.ttf");
    const auto medium = loadFontFile("NotoSansSC-Medium.ttf");
    const auto bold = loadFontFile("NotoSansSC-Bold.ttf");
    REQUIRE(medium->weight() == 500U);

    resonate::ui::detail::TextAtlasBuilder atlas({regular, medium, bold}, 256U);
    std::vector<resonate::ui::detail::GlyphQuad> quads;
    float advance = 0.0F;
    std::string error;
    REQUIRE(atlas.buildRun("A", 24.0F, 500.0F, 0.0F, 0U, false, false, quads, advance, error));
    REQUIRE(quads.size() == 1U);
    CHECK(quads[0].font_index == 1U);

    quads.clear();
    REQUIRE(atlas.buildRun("A", 24.0F, 700.0F, 0.0F, 0U, false, false, quads, advance, error));
    REQUIRE(quads.size() == 1U);
    CHECK(quads[0].font_index == 2U);
}
