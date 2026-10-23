#ifndef RESONATE_UI_GLYPH_RUN_H
#define RESONATE_UI_GLYPH_RUN_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <resonate/text/font.h>

namespace resonate::ui::detail
{

/* One glyph quad in widget-local pixels and its atlas UV rect. */
struct GlyphQuad
{
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float u0 = 0.0F;
    float v0 = 0.0F;
    float u1 = 0.0F;
    float v1 = 0.0F;
    /* The atlas key, so a repack can refresh the UV rect before the run is
       attached to the world. */
    std::uint64_t key = 0U;
};

/* One canvas's shelf-packed SDF atlas. Glyphs are cached by size, weight and
   glyph id, so a repeated run rasterizes each glyph once. The page is RGBA8
   with white RGB and the signed distance in alpha. A full page grows (up to
   2048²) and repacks; pending quads refresh their UVs before attachment. */
class TextAtlasBuilder
{
  public:
    explicit TextAtlasBuilder(std::shared_ptr<const text::Font> font, std::uint32_t size = 512U);

    /* Shapes utf8 at `size` pixels per em and appends the glyph quads for the
       wrapped lines. `width` is the alignment span (the widget's authored
       width); when it is positive and `wrap` is set, lines break at spaces or
       between glyphs, and `align` (0 start, 1 center, 2 end) offsets each line
       within it. False with a named error when the page is full or a glyph
       cannot be rasterized. */
    bool buildRun(std::string_view utf8, float size, float weight, float width, std::uint32_t align,
                  bool wrap, std::vector<GlyphQuad>& out, std::string& out_error);

    /* Refreshes a quad's UV rect after a repack. False when the key is not in
       the atlas. */
    [[nodiscard]] bool updateQuad(GlyphQuad& quad) const;

    [[nodiscard]] bool used() const noexcept { return used_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }

  private:
    struct AtlasGlyph
    {
        GlyphQuad quad;
        float advance = 0.0F;
        std::int32_t bearing_x = 0;
        std::int32_t bearing_y = 0;
        std::vector<std::uint8_t> coverage;
    };

    static constexpr std::uint32_t kMaxSize = 2048U;

    bool getGlyph(std::uint32_t glyph, float size, float weight, AtlasGlyph& out,
                  std::string& out_error);
    bool placeGlyph(AtlasGlyph& glyph);
    bool grow();
    void resetPage(std::uint32_t size);

    std::shared_ptr<const text::Font> font_;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::vector<std::uint8_t> pixels_;
    std::unordered_map<std::uint64_t, AtlasGlyph> glyphs_;
    std::uint32_t cursor_x_ = 1U;
    std::uint32_t cursor_y_ = 1U;
    std::uint32_t row_height_ = 0U;
    bool used_ = false;
};

} // namespace resonate::ui::detail

#endif /* RESONATE_UI_GLYPH_RUN_H */
