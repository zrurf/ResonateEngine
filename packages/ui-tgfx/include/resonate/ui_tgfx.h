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

/* Every draw entry carries the rectangle it is confined to (target pixels);
   a non-positive clip width draws unclipped. `radius` rounds the corners and
   `border` keeps only a ring of that width along the edge. */
typedef struct ResonateTgfxRect
{
    float x;
    float y;
    float width;
    float height;
    float radius;
    float border;
    float r;
    float g;
    float b;
    float a;
    float clip_x;
    float clip_y;
    float clip_width;
    float clip_height;
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
    float clip_x;
    float clip_y;
    float clip_width;
    float clip_height;
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
    float r;
    float g;
    float b;
    float a; /* the tint multiplied into the sample */
    float clip_x;
    float clip_y;
    float clip_width;
    float clip_height;
} ResonateTgfxImageDraw;

/* The renderer's color-attachment-capable image the UI layer draws into, so
   the frame samples it without a readback. Mirrors the render device's
   native_image tail. */
typedef struct ResonateTgfxNativeImage
{
    std::uint64_t image;
    std::uint32_t format;
    std::uint32_t layout;
    std::uint32_t width;
    std::uint32_t height;
} ResonateTgfxNativeImage;

/* Wraps the host's Vulkan handles and opens every font path. The handles are
   borrowed; a path that fails to open fails the call. Answers 0 on success. */
int32_t resonate_tgfx_ui_create(void* instance, void* physical_device, void* device, void* queue,
                                std::uint32_t queue_family, const char* const* font_paths,
                                std::uint32_t font_count, ResonateTgfxUi* out_ui);

/* One draw in the producer's paint order: `kind` is 0 for rects, 1 for texts
   and 2 for images, `index` names the entry in that array. */
typedef struct ResonateTgfxDrawRef
{
    std::uint32_t kind;
    std::uint32_t index;
} ResonateTgfxDrawRef;

/* Draws one UI layer into the wrapper's surface. Answers 0 on success. */
int32_t resonate_tgfx_ui_draw(ResonateTgfxUi ui, std::uint32_t width, std::uint32_t height,
                              const ResonateTgfxRect* rects, std::uint32_t rect_count,
                              const ResonateTgfxText* texts, std::uint32_t text_count,
                              const ResonateTgfxImageDraw* images, std::uint32_t image_count);

/* The same draw in the producer's paint order. Falls back to the batched order
   when `order` is empty. */
int32_t resonate_tgfx_ui_draw_ordered(ResonateTgfxUi ui, std::uint32_t width, std::uint32_t height,
                                      const ResonateTgfxRect* rects, std::uint32_t rect_count,
                                      const ResonateTgfxText* texts, std::uint32_t text_count,
                                      const ResonateTgfxImageDraw* images, std::uint32_t image_count,
                                      const ResonateTgfxDrawRef* order, std::uint32_t order_count);

/* Registers the image file under `id` for draw calls (PNG through tgfx's
   codecs); re-registering replaces. Must not run concurrently with draw.
   Answers 0 on success. */
int32_t resonate_tgfx_image_register(ResonateTgfxUi ui, std::uint32_t id, const char* path);

/* Releases a registered image; draw calls naming it afterwards are no-ops. */
void resonate_tgfx_image_release(ResonateTgfxUi ui, std::uint32_t id);

/* Copies the last draw to `data` as tightly packed premultiplied RGBA8. */
int32_t resonate_tgfx_ui_read_pixels(ResonateTgfxUi ui, void* data, std::uint64_t size_bytes);

/* Draws the UI layer into `target`, the renderer's own color-attachment image,
   instead of a wrapper-owned surface; drawing afterwards without read_pixels
   is the zero-copy path. The image's memory is bound by the render device.
   Answers 0 on success. */
int32_t resonate_tgfx_ui_attach(ResonateTgfxUi ui, const ResonateTgfxNativeImage* target);

/* The layout the last draw left the attached target in; undefined (0) when no
   target is attached. */
std::uint32_t resonate_tgfx_ui_target_layout(ResonateTgfxUi ui);

void resonate_tgfx_ui_destroy(ResonateTgfxUi ui);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_UI_TGFX_H */
