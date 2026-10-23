#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <resonate/text/font.h>

#include "../../packages/ui/src/glyph_run.h"

namespace
{

std::shared_ptr<const resonate::text::Font> loadFont()
{
    std::string error;
    auto font = resonate::text::Font::loadFile(
        std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/ResonateSansSC-Subset.ttf", error);
    REQUIRE(font != nullptr);
    return font;
}

} // namespace

TEST_CASE("ui atlas: a full page grows and repacks its glyphs", "[ui][text][atlas]")
{
    resonate::ui::detail::TextAtlasBuilder atlas(loadFont(), 32U);
    std::vector<resonate::ui::detail::GlyphQuad> quads;
    std::string error;
    REQUIRE(atlas.buildRun(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
        "\xE7\x94\x9F\xE5\x91\xBD\xE5\x80\xBC\xE5\xBC\xB9\xE8\x8D\xAF\xE5\x8A\xA8\xE4\xBD\x9C"
        "\xE6\xBC\x94\xE7\xA4\xBA\xE6\x95\xB0\xE6\x8D\xAE",
        24.0F, 400.0F, 0.0F, 0U, false, quads, error));
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
