#ifndef RESONATE_UI_TGFX_H
#define RESONATE_UI_TGFX_H

/* tgfx behind a small C boundary. tgfx's debug archive is over a gigabyte;
   keeping it inside one DLL lets hosts link a tiny import library. The UI layer
   is rendered into a tgfx-owned surface and read back as premultiplied RGBA8;
   the renderer uploads those pixels into its own UI image. Text arrives as
   laid-out glyph ids and pen origins — shaping stays in the engine. No STL
   crosses the boundary. */

#include <cstdint>

#ifdef __cplusplus
extern "C"
{
#endif

typedef void* ResonateTgfxUi;

typedef struct ResonateTgfxRect
{
    float x;
    float y;
    float width;
    float height;
    float r;
    float g;
    float b;
    float a;
} ResonateTgfxRect;

typedef struct ResonateTgfxGlyph
{
    float x; /* pen origin, target pixels */
    float y; /* baseline */
    std::uint32_t glyph_id;
    std::uint32_t font_index; /* into the create-time font path list */
} ResonateTgfxGlyph;

typedef struct ResonateTgfxText
{
    float size; /* pixels per em */
    float r;
    float g;
    float b;
    float a;
    const ResonateTgfxGlyph* glyphs;
    std::uint32_t glyph_count;
} ResonateTgfxText;

typedef struct ResonateTgfxImageDraw
{
    std::uint32_t id; /* a registered image; unknown ids draw nothing */
    float x;
    float y;
    float width;
    float height;
    float u0;
    float v0;
    float u1;
    float v1; /* the source sub-rect, in image fractions */
} ResonateTgfxImageDraw;

/* Wraps the host's Vulkan handles and opens every font path. The handles are
   borrowed; a path that fails to open fails the call. Answers 0 on success. */
int32_t resonate_tgfx_ui_create(void* instance, void* physical_device, void* device, void* queue,
                                std::uint32_t queue_family, const char* const* font_paths,
                                std::uint32_t font_count, ResonateTgfxUi* out_ui);

/* Draws one UI layer into the wrapper's surface. Answers 0 on success. */
int32_t resonate_tgfx_ui_draw(ResonateTgfxUi ui, std::uint32_t width, std::uint32_t height,
                              const ResonateTgfxRect* rects, std::uint32_t rect_count,
                              const ResonateTgfxText* texts, std::uint32_t text_count,
                              const ResonateTgfxImageDraw* images, std::uint32_t image_count);

/* Registers the image file under `id` for draw calls (PNG through tgfx's
   codecs); re-registering replaces. Must not run concurrently with draw.
   Answers 0 on success. */
int32_t resonate_tgfx_image_register(ResonateTgfxUi ui, std::uint32_t id, const char* path);

/* Releases a registered image; draw calls naming it afterwards are no-ops. */
void resonate_tgfx_image_release(ResonateTgfxUi ui, std::uint32_t id);

/* Copies the last draw to `data` as tightly packed premultiplied RGBA8. */
int32_t resonate_tgfx_ui_read_pixels(ResonateTgfxUi ui, void* data, std::uint64_t size_bytes);

void resonate_tgfx_ui_destroy(ResonateTgfxUi ui);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_UI_TGFX_H */
