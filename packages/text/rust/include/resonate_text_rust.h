#ifndef RESONATE_TEXT_RUST_H
#define RESONATE_TEXT_RUST_H

#include <stddef.h>
#include <stdint.h>

/* The C boundary of the Rust font island. Skrifa parses and instantiates the
   font; zeno rasterizes the outline. Raster bitmaps are 8-bit coverage in
   top-left row order and carry their placement relative to the pen. */
#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateTextFont ResonateTextFont;

typedef struct ResonateTextBitmap
{
    uint32_t width;
    uint32_t height;
    int32_t bearing_x;
    int32_t bearing_y;
    float advance;
    uint8_t* pixels;
} ResonateTextBitmap;

ResonateTextFont* resonate_text_font_create(const uint8_t* data, size_t len);
void resonate_text_font_destroy(ResonateTextFont* font);

/* 1 on success; the metrics are in pixels for the requested ppem. */
int32_t resonate_text_font_metrics(const ResonateTextFont* font, float size, float* ascent,
                                   float* descent, float* leading);

uint32_t resonate_text_font_glyph_index(const ResonateTextFont* font, uint32_t codepoint);

/* 1 on success. A glyph with no outline answers success with a zero-size
   bitmap and its advance. */
int32_t resonate_text_glyph_raster(const ResonateTextFont* font, uint32_t glyph_id, float size,
                                   ResonateTextBitmap* out);

void resonate_text_bitmap_free(ResonateTextBitmap* bitmap);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* RESONATE_TEXT_RUST_H */
