#ifndef RESONATE_UI_TGFX_H
#define RESONATE_UI_TGFX_H

/* tgfx behind a small C boundary. tgfx's debug archive is over a gigabyte;
   keeping it inside one DLL lets hosts link a tiny import library. The UI layer
   is rendered into a tgfx-owned surface and read back as premultiplied RGBA8;
   the renderer uploads those pixels into its own UI image. No STL crosses the
   boundary. */

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

typedef struct ResonateTgfxText
{
    float x;
    float y;
    float width;
    float height;
    float size;
    float r;
    float g;
    float b;
    float a;
    std::uint16_t weight;
    std::uint8_t align; /* 0 start, 1 center, 2 end */
    const char* utf8;
    std::uint32_t length;
} ResonateTgfxText;

/* Wraps the host's Vulkan handles. The handles are borrowed. Answers 0 on
   success. */
int32_t resonate_tgfx_ui_create(void* instance, void* physical_device, void* device, void* queue,
                                std::uint32_t queue_family, const char* font_path,
                                ResonateTgfxUi* out_ui);

/* Draws one UI layer into the wrapper's surface. Answers 0 on success. */
int32_t resonate_tgfx_ui_draw(ResonateTgfxUi ui, std::uint32_t width, std::uint32_t height,
                              const ResonateTgfxRect* rects, std::uint32_t rect_count,
                              const ResonateTgfxText* texts, std::uint32_t text_count);

/* Copies the last draw to `data` as tightly packed premultiplied RGBA8. */
int32_t resonate_tgfx_ui_read_pixels(ResonateTgfxUi ui, void* data, std::uint64_t size_bytes);

void resonate_tgfx_ui_destroy(ResonateTgfxUi ui);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_UI_TGFX_H */
