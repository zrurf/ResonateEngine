#ifndef RESONATE_TEXT_FONT_H
#define RESONATE_TEXT_FONT_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/*
 * The text stack's face and shaping surface.
 *
 * A Font owns the font bytes, the Rust/Skrifa face and a HarfBuzz face. It is
 * immutable after loading and shared; shaping creates its own HarfBuzz font at
 * the requested size, so concurrent shape calls are safe. Glyph ids are the
 * font's own ids and are what both HarfBuzz and Skrifa address.
 */
namespace resonate::text
{

struct FontMetrics
{
    float ascent = 0.0F;
    float descent = 0.0F;
    float leading = 0.0F;
};

struct ShapedGlyph
{
    std::uint32_t glyph = 0;
    std::uint32_t cluster = 0;
    float x_advance = 0.0F;
    float y_advance = 0.0F;
    float x_offset = 0.0F;
    float y_offset = 0.0F;
};

struct GlyphBitmap
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /* Distance from the pen to the bitmap's top-left, in pixels; y is up. */
    std::int32_t bearing_x = 0;
    std::int32_t bearing_y = 0;
    float advance = 0.0F;
    std::vector<std::uint8_t> coverage; /* width * height, top-left row order */
};

class Font
{
  public:
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    /* Null with a named error when the file cannot be read or the bytes are
       not a font Skrifa can parse. */
    [[nodiscard]] static std::shared_ptr<Font> loadFile(const std::string& path,
                                                        std::string& out_error);
    [[nodiscard]] static std::shared_ptr<Font> fromMemory(std::vector<std::uint8_t> bytes,
                                                          std::string& out_error);

    [[nodiscard]] bool metrics(float size, FontMetrics& out) const;
    [[nodiscard]] std::uint32_t glyphIndex(std::uint32_t codepoint) const;

    /* The glyph's coverage bitmap at `size` pixels per em. A blank glyph (a
       space) answers true with an empty bitmap and its advance. */
    [[nodiscard]] bool rasterize(std::uint32_t glyph, float size, GlyphBitmap& out) const;

  private:
    Font() = default;
    struct Impl;
    friend bool shape(const Font& font, std::string_view utf8, float size,
                      std::vector<ShapedGlyph>& out, std::string& out_error);
    std::unique_ptr<Impl> impl_;
};

/* Shapes UTF-8 into positioned glyphs at `size` pixels per em. The result is
   in font order with advances already scaled to pixels; the caller draws each
   glyph at its pen plus the reported offsets. */
[[nodiscard]] bool shape(const Font& font, std::string_view utf8, float size,
                         std::vector<ShapedGlyph>& out, std::string& out_error);

} // namespace resonate::text

#endif /* RESONATE_TEXT_FONT_H */
