#include <resonate/text/font.h>

#include <climits>
#include <cstring>
#include <fstream>
#include <iterator>

#include <hb.h>

#include "resonate_text_rust.h"

namespace resonate::text
{

struct Font::Impl
{
    std::vector<std::uint8_t> bytes;
    ResonateTextFont* rust = nullptr;
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;

    ~Impl()
    {
        if (face != nullptr)
        {
            hb_face_destroy(face);
        }
        if (blob != nullptr)
        {
            hb_blob_destroy(blob);
        }
        if (rust != nullptr)
        {
            resonate_text_font_destroy(rust);
        }
    }
};

Font::~Font() = default;

std::shared_ptr<Font> Font::fromMemory(std::vector<std::uint8_t> bytes, std::string& out_error)
{
    if (bytes.empty())
    {
        out_error = "the font is empty";
        return nullptr;
    }

    std::shared_ptr<Font> font(new Font());
    font->impl_ = std::make_unique<Impl>();
    Impl& impl = *font->impl_;
    impl.bytes = std::move(bytes);
    impl.rust = resonate_text_font_create(impl.bytes.data(), impl.bytes.size());
    if (impl.rust == nullptr)
    {
        out_error = "the bytes are not a font";
        return nullptr;
    }

    impl.blob = hb_blob_create(reinterpret_cast<const char*>(impl.bytes.data()),
                               static_cast<unsigned int>(impl.bytes.size()), HB_MEMORY_MODE_READONLY,
                               nullptr, nullptr);
    impl.face = hb_face_create(impl.blob, 0);
    if (impl.face == nullptr || hb_face_get_glyph_count(impl.face) == 0U)
    {
        out_error = "HarfBuzz could not open the face";
        return nullptr;
    }
    return font;
}

std::shared_ptr<Font> Font::loadFile(const std::string& path, std::string& out_error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        out_error = "cannot read " + path;
        return nullptr;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof())
    {
        out_error = "cannot read " + path;
        return nullptr;
    }
    return fromMemory(std::move(bytes), out_error);
}

bool Font::metrics(float size, FontMetrics& out) const
{
    if (impl_ == nullptr || impl_->rust == nullptr || !(size > 0.0F))
    {
        return false;
    }
    return resonate_text_font_metrics(impl_->rust, size, &out.ascent, &out.descent,
                                      &out.leading) != 0;
}

std::uint32_t Font::glyphIndex(std::uint32_t codepoint) const
{
    if (impl_ == nullptr || impl_->rust == nullptr)
    {
        return 0U;
    }
    return resonate_text_font_glyph_index(impl_->rust, codepoint);
}

bool Font::rasterize(std::uint32_t glyph, float size, GlyphBitmap& out) const
{
    if (impl_ == nullptr || impl_->rust == nullptr || !(size > 0.0F))
    {
        return false;
    }
    ResonateTextBitmap bitmap{};
    if (resonate_text_glyph_raster(impl_->rust, glyph, size, &bitmap) == 0)
    {
        return false;
    }
    out.width = bitmap.width;
    out.height = bitmap.height;
    out.bearing_x = bitmap.bearing_x;
    out.bearing_y = bitmap.bearing_y;
    out.advance = bitmap.advance;
    out.coverage.clear();
    if (bitmap.pixels != nullptr && bitmap.width != 0U && bitmap.height != 0U)
    {
        const std::size_t count = static_cast<std::size_t>(bitmap.width) * bitmap.height;
        out.coverage.assign(bitmap.pixels, bitmap.pixels + count);
    }
    resonate_text_bitmap_free(&bitmap);
    return true;
}

bool shape(const Font& font, std::string_view utf8, float size, std::vector<ShapedGlyph>& out,
           std::string& out_error)
{
    out.clear();
    if (font.impl_ == nullptr || font.impl_->face == nullptr || !(size > 0.0F))
    {
        out_error = "the font is not usable";
        return false;
    }
    if (utf8.size() > static_cast<std::size_t>(INT_MAX))
    {
        out_error = "the text is too long";
        return false;
    }

    hb_font_t* hb_font = hb_font_create(font.impl_->face);
    if (hb_font == nullptr)
    {
        out_error = "HarfBuzz could not create the font";
        return false;
    }
    const int scale = static_cast<int>(size * 64.0F + 0.5F);
    hb_font_set_scale(hb_font, scale, scale);

    hb_buffer_t* buffer = hb_buffer_create();
    if (buffer == nullptr)
    {
        hb_font_destroy(hb_font);
        out_error = "HarfBuzz could not create the buffer";
        return false;
    }
    hb_buffer_add_utf8(buffer, utf8.data(), static_cast<int>(utf8.size()), 0,
                       static_cast<int>(utf8.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(hb_font, buffer, nullptr, 0);

    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    if (count == 0U)
    {
        hb_buffer_destroy(buffer);
        hb_font_destroy(hb_font);
        return true;
    }
    if (infos == nullptr || positions == nullptr)
    {
        hb_buffer_destroy(buffer);
        hb_font_destroy(hb_font);
        out_error = "HarfBuzz returned no shaped glyphs";
        return false;
    }

    out.reserve(count);
    for (unsigned int index = 0; index < count; ++index)
    {
        ShapedGlyph glyph;
        glyph.glyph = infos[index].codepoint;
        glyph.cluster = infos[index].cluster;
        glyph.x_advance = static_cast<float>(positions[index].x_advance) / 64.0F;
        glyph.y_advance = static_cast<float>(positions[index].y_advance) / 64.0F;
        glyph.x_offset = static_cast<float>(positions[index].x_offset) / 64.0F;
        glyph.y_offset = static_cast<float>(positions[index].y_offset) / 64.0F;
        out.push_back(glyph);
    }

    hb_buffer_destroy(buffer);
    hb_font_destroy(hb_font);
    return true;
}

} // namespace resonate::text
