#include "glyph_run.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace resonate::ui::detail
{
namespace
{

constexpr std::uint32_t kGlyphPadding = 1U;

std::uint64_t glyphKey(std::uint32_t font, std::uint32_t glyph, float size, float weight)
{
    const auto scaled = static_cast<std::uint32_t>(std::lround(size * 64.0F));
    const auto weighted = static_cast<std::uint32_t>(std::lround(weight));
    return (static_cast<std::uint64_t>(font & 0xFFU) << 56U) |
           (static_cast<std::uint64_t>(scaled & 0xFFFFFU) << 36U) |
           (static_cast<std::uint64_t>(weighted & 0xFFFFU) << 20U) |
           static_cast<std::uint64_t>(glyph & 0xFFFFFU);
}

std::size_t codepointLength(std::string_view utf8, std::size_t offset)
{
    const auto first = static_cast<unsigned char>(utf8[offset]);
    if (first < 0x80U)
    {
        return 1U;
    }
    if ((first & 0xE0U) == 0xC0U)
    {
        return 2U;
    }
    if ((first & 0xF0U) == 0xE0U)
    {
        return 3U;
    }
    if ((first & 0xF8U) == 0xF0U)
    {
        return 4U;
    }
    return 1U;
}

std::uint32_t codepointAt(std::string_view utf8, std::uint32_t cluster)
{
    if (cluster >= utf8.size())
    {
        return 0U;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(utf8.data() + cluster);
    const unsigned char first = bytes[0];
    if (first < 0x80U)
    {
        return first;
    }
    if ((first & 0xE0U) == 0xC0U && cluster + 1U < utf8.size())
    {
        return ((first & 0x1FU) << 6U) | (bytes[1] & 0x3FU);
    }
    if ((first & 0xF0U) == 0xE0U && cluster + 2U < utf8.size())
    {
        return ((first & 0x0FU) << 12U) | ((bytes[1] & 0x3FU) << 6U) | (bytes[2] & 0x3FU);
    }
    if ((first & 0xF8U) == 0xF0U && cluster + 3U < utf8.size())
    {
        return ((first & 0x07U) << 18U) | ((bytes[1] & 0x3FU) << 12U) |
               ((bytes[2] & 0x3FU) << 6U) | (bytes[3] & 0x3FU);
    }
    return first;
}

} // namespace

TextAtlasBuilder::TextAtlasBuilder(std::vector<std::shared_ptr<const text::Font>> fonts,
                                   std::uint32_t size)
    : fonts_(std::move(fonts))
{
    resetPage(size);
}

void TextAtlasBuilder::resetPage(std::uint32_t size)
{
    width_ = size;
    height_ = size;
    pixels_.assign(static_cast<std::size_t>(size) * size * 4U, 0U);
    /* Empty texels are transparent white, so a filter tap that reaches into
       them cannot darken the glyph's ink. */
    for (std::size_t index = 0; index < pixels_.size(); index += 4U)
    {
        pixels_[index] = 255U;
        pixels_[index + 1U] = 255U;
        pixels_[index + 2U] = 255U;
    }
    cursor_x_ = kGlyphPadding;
    cursor_y_ = kGlyphPadding;
    row_height_ = 0U;
    used_ = false;
}

bool TextAtlasBuilder::placeGlyph(AtlasGlyph& glyph)
{
    const auto width = static_cast<std::uint32_t>(glyph.quad.width);
    const auto height = static_cast<std::uint32_t>(glyph.quad.height);
    std::uint32_t x = cursor_x_;
    std::uint32_t y = cursor_y_;
    std::uint32_t row_height = row_height_;
    if (x + width + kGlyphPadding > width_)
    {
        x = kGlyphPadding;
        y = cursor_y_ + row_height_ + kGlyphPadding;
        row_height = 0U;
    }
    if (y + height + kGlyphPadding > height_)
    {
        return false;
    }

    for (std::uint32_t row = 0; row < height; ++row)
    {
        const std::size_t source = static_cast<std::size_t>(row) * width;
        const std::size_t destination = (static_cast<std::size_t>(y + row) * width_ + x) * 4U;
        for (std::uint32_t column = 0; column < width; ++column)
        {
            pixels_[destination + column * 4U + 3U] = glyph.coverage[source + column];
        }
    }

    glyph.quad.u0 = static_cast<float>(x) / static_cast<float>(width_);
    glyph.quad.v0 = static_cast<float>(y) / static_cast<float>(height_);
    glyph.quad.u1 = static_cast<float>(x + width) / static_cast<float>(width_);
    glyph.quad.v1 = static_cast<float>(y + height) / static_cast<float>(height_);
    cursor_x_ = x + width + kGlyphPadding;
    cursor_y_ = y;
    row_height_ = std::max(row_height, height);
    used_ = true;
    return true;
}

bool TextAtlasBuilder::grow()
{
    std::uint32_t size = width_ < kMaxSize ? width_ * 2U : kMaxSize;
    if (size > kMaxSize)
    {
        size = kMaxSize;
    }
    if (size <= width_)
    {
        return false;
    }
    resetPage(size);
    for (auto& entry : glyphs_)
    {
        if (entry.second.quad.width > 0.0F && !placeGlyph(entry.second))
        {
            return grow();
        }
    }
    return true;
}

bool TextAtlasBuilder::updateQuad(GlyphQuad& quad) const
{
    const auto found = glyphs_.find(quad.key);
    if (found == glyphs_.end())
    {
        return false;
    }
    quad.u0 = found->second.quad.u0;
    quad.v0 = found->second.quad.v0;
    quad.u1 = found->second.quad.u1;
    quad.v1 = found->second.quad.v1;
    return true;
}

bool TextAtlasBuilder::getGlyph(std::uint32_t font, std::uint32_t glyph, float size,
                                float weight, AtlasGlyph& out, std::string& out_error)
{
    if (font >= fonts_.size() || fonts_[font] == nullptr)
    {
        out_error = "the text has no font";
        return false;
    }
    const std::uint64_t key = glyphKey(font, glyph, size, weight);
    if (const auto found = glyphs_.find(key); found != glyphs_.end())
    {
        out = found->second;
        return true;
    }

    text::GlyphBitmap bitmap;
    if (!fonts_[font]->rasterize(glyph, size, weight, bitmap))
    {
        out_error = "glyph rasterization failed";
        return false;
    }

    AtlasGlyph entry;
    entry.advance = bitmap.advance;
    entry.quad.key = key;
    if (bitmap.width != 0U && bitmap.height != 0U)
    {
        entry.quad.width = static_cast<float>(bitmap.width);
        entry.quad.height = static_cast<float>(bitmap.height);
        entry.bearing_x = bitmap.bearing_x;
        entry.bearing_y = bitmap.bearing_y;
        entry.coverage = std::move(bitmap.coverage);
        while (!placeGlyph(entry))
        {
            if (!grow())
            {
                out_error = "the font atlas is full";
                return false;
            }
        }
    }

    glyphs_.emplace(key, entry);
    out = entry;
    return true;
}

bool TextAtlasBuilder::buildRun(std::string_view utf8, float size, float weight, float width,
                                std::uint32_t align, bool wrap, std::vector<GlyphQuad>& out,
                                std::string& out_error)
{
    if (fonts_.empty() || fonts_[0] == nullptr || !(size > 0.0F))
    {
        out_error = "the text has no font";
        return false;
    }

    struct RunGlyph
    {
        text::ShapedGlyph glyph;
        std::uint32_t font = 0U;
    };
    std::vector<RunGlyph> shaped;
    std::size_t segment_begin = 0U;
    std::uint32_t segment_font = 0U;
    bool have_segment = false;
    for (std::size_t offset = 0; offset < utf8.size();)
    {
        const std::size_t length = codepointLength(utf8, offset);
        const std::uint32_t codepoint = codepointAt(utf8, static_cast<std::uint32_t>(offset));
        std::uint32_t chosen = 0U;
        for (std::uint32_t font = 0; font < fonts_.size(); ++font)
        {
            if (fonts_[font] != nullptr && fonts_[font]->glyphIndex(codepoint) != 0U)
            {
                chosen = font;
                break;
            }
        }
        if (!have_segment)
        {
            segment_font = chosen;
            have_segment = true;
        }
        else if (chosen != segment_font)
        {
            std::vector<text::ShapedGlyph> piece;
            if (!text::shape(*fonts_[segment_font],
                             utf8.substr(segment_begin, offset - segment_begin), size, weight, piece,
                             out_error))
            {
                return false;
            }
            for (text::ShapedGlyph glyph : piece)
            {
                glyph.cluster += static_cast<std::uint32_t>(segment_begin);
                shaped.push_back(RunGlyph{glyph, segment_font});
            }
            segment_begin = offset;
            segment_font = chosen;
        }
        offset += length;
    }
    if (have_segment && segment_begin < utf8.size())
    {
        std::vector<text::ShapedGlyph> piece;
        if (!text::shape(*fonts_[segment_font], utf8.substr(segment_begin), size, weight, piece,
                         out_error))
        {
            return false;
        }
        for (text::ShapedGlyph glyph : piece)
        {
            glyph.cluster += static_cast<std::uint32_t>(segment_begin);
            shaped.push_back(RunGlyph{glyph, segment_font});
        }
    }
    if (shaped.empty())
    {
        return true;
    }

    text::FontMetrics metrics;
    if (!fonts_[0]->metrics(size, weight, metrics))
    {
        out_error = "font metrics are unavailable";
        return false;
    }
    float line_height = metrics.ascent - metrics.descent + metrics.leading;
    if (!(line_height > 0.0F))
    {
        line_height = size * 1.2F;
    }

    struct LineGlyph
    {
        text::ShapedGlyph glyph;
        AtlasGlyph atlas;
        bool space = false;
    };

    std::vector<LineGlyph> line;
    float pen = 0.0F;
    float line_width = 0.0F;
    std::int32_t last_space = -1;
    std::uint32_t line_index = 0U;
    auto refresh = [&]
    {
        line_width = 0.0F;
        last_space = -1;
        for (std::size_t index = 0; index < line.size(); ++index)
        {
            line_width += line[index].glyph.x_advance;
            if (line[index].space)
            {
                last_space = static_cast<std::int32_t>(index);
            }
        }
    };
    auto flush = [&](std::size_t count, bool force)
    {
        if (count == 0U && !force)
        {
            return;
        }
        const float offset = width > 0.0F && align != 0U
                                 ? (align == 1U ? (width - line_width) * 0.5F : width - line_width)
                                 : 0.0F;
        for (std::size_t index = 0; index < count; ++index)
        {
            const LineGlyph& entry = line[index];
            if (entry.atlas.quad.width > 0.0F && entry.atlas.quad.height > 0.0F)
            {
                GlyphQuad quad = entry.atlas.quad;
                quad.x = offset + pen + entry.glyph.x_offset +
                         static_cast<float>(entry.atlas.bearing_x);
                quad.y = metrics.ascent + static_cast<float>(line_index) * line_height -
                         entry.glyph.y_offset - static_cast<float>(entry.atlas.bearing_y);
                out.push_back(quad);
            }
            pen += entry.glyph.x_advance;
        }
        line.erase(line.begin(), line.begin() + static_cast<std::ptrdiff_t>(count));
        if (count > 0U || force)
        {
            ++line_index;
        }
        pen = 0.0F;
        refresh();
    };

    for (const RunGlyph& run : shaped)
    {
        const text::ShapedGlyph& glyph = run.glyph;
        AtlasGlyph atlas;
        if (!getGlyph(run.font, glyph.glyph, size, weight, atlas, out_error))
        {
            return false;
        }
        const std::uint32_t codepoint = codepointAt(utf8, glyph.cluster);
        if (codepoint == '\n' || codepoint == '\r')
        {
            flush(line.size(), true);
            continue;
        }
        const bool space = codepoint == ' ' || codepoint == '\t';
        const float advance = glyph.x_advance;
        if (wrap && width > 0.0F && !line.empty() && line_width + advance > width)
        {
            if (last_space >= 0)
            {
                flush(static_cast<std::size_t>(last_space), true);
                if (!line.empty() && line.front().space)
                {
                    line.erase(line.begin());
                    refresh();
                }
            }
            else
            {
                flush(line.size(), true);
            }
        }
        line.push_back(LineGlyph{glyph, atlas, space});
        line_width += advance;
        if (space)
        {
            last_space = static_cast<std::int32_t>(line.size() - 1U);
        }
    }
    flush(line.size(), false);
    return true;
}

} // namespace resonate::ui::detail
