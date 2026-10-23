#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <resonate/text/font.h>

namespace
{

std::string fontPath()
{
    return std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/ResonateSansSC-Subset.ttf";
}

} // namespace

TEST_CASE("text: the sample font shapes latin and CJK", "[text]")
{
    std::string error;
    const auto font = resonate::text::Font::loadFile(fontPath(), error);
    REQUIRE(font != nullptr);
    CHECK(error.empty());

    resonate::text::FontMetrics metrics;
    REQUIRE(font->metrics(32.0F, metrics));
    CHECK(metrics.ascent > 0.0F);
    CHECK(metrics.descent < 0.0F);

    CHECK(font->glyphIndex('A') != 0U);
    CHECK(font->glyphIndex(0x4F60U) != 0U); /* 你 */

    std::vector<resonate::text::ShapedGlyph> glyphs;
    REQUIRE(resonate::text::shape(*font, "Hi \xE4\xBD\xA0\xE5\xA5\xBD", 32.0F, glyphs, error));
    REQUIRE(glyphs.size() == 5U);
    CHECK(glyphs[0].glyph != 0U);
    CHECK(glyphs[3].glyph != 0U);
    CHECK(glyphs[0].x_advance > 0.0F);
    CHECK(glyphs[3].x_advance > 0.0F);

    std::vector<resonate::text::ShapedGlyph> empty;
    REQUIRE(resonate::text::shape(*font, "", 32.0F, empty, error));
    CHECK(empty.empty());
}

TEST_CASE("text: a glyph rasterizes into a placement bitmap", "[text]")
{
    std::string error;
    const auto font = resonate::text::Font::loadFile(fontPath(), error);
    REQUIRE(font != nullptr);

    resonate::text::GlyphBitmap glyph;
    REQUIRE(font->rasterize(font->glyphIndex('A'), 32.0F, glyph));
    CHECK(glyph.width > 0U);
    CHECK(glyph.height > 0U);
    CHECK(glyph.bearing_y > 0);
    CHECK(glyph.advance > 0.0F);
    REQUIRE(glyph.coverage.size() == static_cast<std::size_t>(glyph.width) * glyph.height);
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    for (const std::uint8_t value : glyph.coverage)
    {
        low += value < 64U ? 1U : 0U;
        high += value > 160U ? 1U : 0U;
    }
    /* A signed distance field spans both sides of the edge. */
    CHECK(low > 10U);
    CHECK(high > 10U);

    /* A space has an advance but no outline. */
    resonate::text::GlyphBitmap space;
    REQUIRE(font->rasterize(font->glyphIndex(' '), 32.0F, space));
    CHECK(space.width == 0U);
    CHECK(space.height == 0U);
    CHECK(space.coverage.empty());
    CHECK(space.advance > 0.0F);
}

TEST_CASE("text: bad font bytes are refused", "[text]")
{
    std::string error;
    const std::vector<std::uint8_t> bytes(64U, 0x42U);
    CHECK(resonate::text::Font::fromMemory(bytes, error) == nullptr);
    CHECK_FALSE(error.empty());
}
