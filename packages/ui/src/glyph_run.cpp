#include "glyph_run.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace resonate::ui::detail
{
namespace
{

constexpr std::uint32_t kGlyphPadding = 1U;

std::uint64_t glyphKey(std::uint32_t glyph, float size)
{
    const auto scaled = static_cast<std::uint32_t>(std::lround(size * 64.0F));
    return (static_cast<std::uint64_t>(scaled) << 32U) | glyph;
}

} // namespace

TextAtlasBuilder::TextAtlasBuilder(std::shared_ptr<const text::Font> font, std::uint32_t size)
    : font_(std::move(font)), width_(size), height_(size),
      pixels_(static_cast<std::size_t>(size) * size * 4U, 0U)
{
    /* Empty texels are transparent white, so a filter tap that reaches into
       them cannot darken the glyph's ink. */
    for (std::size_t index = 0; index < pixels_.size(); index += 4U)
    {
        pixels_[index] = 255U;
        pixels_[index + 1U] = 255U;
        pixels_[index + 2U] = 255U;
    }
}

bool TextAtlasBuilder::getGlyph(std::uint32_t glyph, float size, AtlasGlyph& out,
                                std::string& out_error)
{
    const std::uint64_t key = glyphKey(glyph, size);
    if (const auto found = glyphs_.find(key); found != glyphs_.end())
    {
        out = found->second;
        return true;
    }

    text::GlyphBitmap bitmap;
    if (!font_->rasterize(glyph, size, bitmap))
    {
        out_error = "glyph rasterization failed";
        return false;
    }

    AtlasGlyph entry;
    entry.advance = bitmap.advance;
    if (bitmap.width != 0U && bitmap.height != 0U)
    {
        if (cursor_x_ + bitmap.width + kGlyphPadding > width_)
        {
            cursor_x_ = kGlyphPadding;
            cursor_y_ += row_height_ + kGlyphPadding;
            row_height_ = 0U;
        }
        if (cursor_y_ + bitmap.height + kGlyphPadding > height_)
        {
            out_error = "the font atlas is full";
            return false;
        }

        for (std::uint32_t row = 0; row < bitmap.height; ++row)
        {
            const std::size_t source = static_cast<std::size_t>(row) * bitmap.width;
            const std::size_t destination =
                (static_cast<std::size_t>(cursor_y_ + row) * width_ + cursor_x_) * 4U;
            for (std::uint32_t column = 0; column < bitmap.width; ++column)
            {
                std::uint8_t* texel = pixels_.data() + destination + column * 4U;
                texel[0] = 255U;
                texel[1] = 255U;
                texel[2] = 255U;
                texel[3] = bitmap.coverage[source + column];
            }
        }

        entry.quad.width = static_cast<float>(bitmap.width);
        entry.quad.height = static_cast<float>(bitmap.height);
        entry.bearing_x = bitmap.bearing_x;
        entry.bearing_y = bitmap.bearing_y;
        entry.quad.u0 = static_cast<float>(cursor_x_) / static_cast<float>(width_);
        entry.quad.v0 = static_cast<float>(cursor_y_) / static_cast<float>(height_);
        entry.quad.u1 = static_cast<float>(cursor_x_ + bitmap.width) / static_cast<float>(width_);
        entry.quad.v1 = static_cast<float>(cursor_y_ + bitmap.height) / static_cast<float>(height_);

        cursor_x_ += bitmap.width + kGlyphPadding;
        row_height_ = std::max(row_height_, bitmap.height);
        used_ = true;
    }

    glyphs_.emplace(key, entry);
    out = entry;
    return true;
}

bool TextAtlasBuilder::buildRun(std::string_view utf8, float size, std::vector<GlyphQuad>& out,
                                std::string& out_error)
{
    if (font_ == nullptr || !(size > 0.0F))
    {
        out_error = "the text has no font";
        return false;
    }

    std::vector<text::ShapedGlyph> shaped;
    if (!text::shape(*font_, utf8, size, shaped, out_error))
    {
        return false;
    }
    if (shaped.empty())
    {
        return true;
    }

    text::FontMetrics metrics;
    if (!font_->metrics(size, metrics))
    {
        out_error = "font metrics are unavailable";
        return false;
    }

    float pen = 0.0F;
    for (const text::ShapedGlyph& glyph : shaped)
    {
        AtlasGlyph atlas;
        if (!getGlyph(glyph.glyph, size, atlas, out_error))
        {
            return false;
        }
        if (atlas.quad.width > 0.0F && atlas.quad.height > 0.0F)
        {
            GlyphQuad quad = atlas.quad;
            quad.x = pen + glyph.x_offset + static_cast<float>(atlas.bearing_x);
            quad.y = metrics.ascent - glyph.y_offset - static_cast<float>(atlas.bearing_y);
            out.push_back(quad);
        }
        pen += glyph.x_advance;
    }
    return true;
}

} // namespace resonate::ui::detail
