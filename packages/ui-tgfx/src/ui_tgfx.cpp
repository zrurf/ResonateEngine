#include <resonate/ui_tgfx.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <tgfx/core/Canvas.h>
#include <tgfx/core/Color.h>
#include <tgfx/core/Data.h>
#include <tgfx/core/Font.h>
#include <tgfx/core/Image.h>
#include <tgfx/core/ImageCodec.h>
#include <tgfx/core/ImageInfo.h>
#include <tgfx/core/Paint.h>
#include <tgfx/core/Surface.h>
#include <tgfx/core/TextBlob.h>
#include <tgfx/core/Typeface.h>
#include <tgfx/gpu/Backend.h>
#include <tgfx/gpu/Context.h>
#include <tgfx/gpu/vulkan/VulkanDevice.h>
#include <tgfx/gpu/vulkan/VulkanTypes.h>

#if defined(_WIN32)
#    define RESONATE_TGFX_API __declspec(dllexport)
#else
/* The Android link hides symbols by default; the C ABI surface must stay
   visible for the host to resolve it across the shared boundary. */
#    define RESONATE_TGFX_API __attribute__((visibility("default")))
#endif

namespace
{

struct Ui
{
    std::shared_ptr<tgfx::VulkanDevice> device;
    std::shared_ptr<tgfx::Surface> surface;
    std::vector<std::shared_ptr<tgfx::Typeface>> typefaces;
    std::unordered_map<std::uint32_t, std::shared_ptr<tgfx::Image>> images;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    /* The layout the attached target was left in; 0 = undefined. */
    std::uint32_t target_layout = 0U;
    /* True once the surface wraps the renderer's own image; the draw then
       never recreates the surface on a size change (a resize re-attaches). */
    bool attached = false;
};

/* One TextBlob per typeface: glyphs of a run may fall back across fonts, and
   a blob carries exactly one. */
void drawText(tgfx::Canvas* canvas, const ResonateTgfxText& run,
              const std::vector<std::shared_ptr<tgfx::Typeface>>& typefaces)
{
    if (run.glyphs == nullptr || run.glyph_count == 0U || !(run.size > 0.0F))
    {
        return;
    }
    tgfx::Paint paint;
    paint.setColor(tgfx::Color{run.r, run.g, run.b, run.a});
    for (std::uint32_t font = 0; font < typefaces.size(); ++font)
    {
        std::vector<tgfx::GlyphID> ids;
        std::vector<tgfx::Point> points;
        for (std::uint32_t index = 0; index < run.glyph_count; ++index)
        {
            const ResonateTgfxGlyph& glyph = run.glyphs[index];
            if (glyph.font_index != font)
            {
                continue;
            }
            ids.push_back(static_cast<tgfx::GlyphID>(glyph.glyph_id));
            points.push_back(tgfx::Point::Make(glyph.x, glyph.y));
        }
        if (ids.empty())
        {
            continue;
        }
        const tgfx::Font tgfx_font(typefaces[font], run.size);
        std::shared_ptr<tgfx::TextBlob> blob =
            tgfx::TextBlob::MakeFrom(ids.data(), points.data(), ids.size(), tgfx_font);
        if (blob != nullptr)
        {
            canvas->drawTextBlob(blob, 0.0F, 0.0F, paint);
        }
    }
}

} // namespace

extern "C"
{

RESONATE_TGFX_API int32_t resonate_tgfx_ui_create(void* instance, void* physical_device,
                                                  void* device, void* queue,
                                                  std::uint32_t queue_family,
                                                  const char* const* font_paths,
                                                  std::uint32_t font_count, ResonateTgfxUi* out_ui)
{
    if (instance == nullptr || physical_device == nullptr || device == nullptr || queue == nullptr ||
        out_ui == nullptr || font_paths == nullptr || font_count == 0U)
    {
        return -1;
    }
    auto ui = std::make_unique<Ui>();
    ui->device = tgfx::VulkanDevice::MakeFrom(instance, physical_device, device, queue,
                                              queue_family);
    if (ui->device == nullptr)
    {
        return -2;
    }
    ui->typefaces.reserve(font_count);
    for (std::uint32_t index = 0; index < font_count; ++index)
    {
        auto typeface = tgfx::Typeface::MakeFromPath(font_paths[index]);
        if (typeface == nullptr)
        {
            ui->device.reset();
            return -2;
        }
        ui->typefaces.push_back(std::move(typeface));
    }
    *out_ui = ui.release();
    return 0;
}

RESONATE_TGFX_API int32_t resonate_tgfx_ui_draw(ResonateTgfxUi handle, std::uint32_t width,
                                                std::uint32_t height,
                                                const ResonateTgfxRect* rects,
                                                std::uint32_t rect_count,
                                                const ResonateTgfxText* texts,
                                                std::uint32_t text_count,
                                                const ResonateTgfxImageDraw* images,
                                                std::uint32_t image_count)
{
    auto* ui = static_cast<Ui*>(handle);
    if (ui == nullptr || width == 0U || height == 0U)
    {
        return -1;
    }
    auto* context = ui->device->lockContext();
    if (context == nullptr)
    {
        return -3;
    }
    if (!ui->attached &&
        (ui->surface == nullptr || ui->width != width || ui->height != height))
    {
        ui->surface = tgfx::Surface::Make(context, static_cast<int>(width),
                                          static_cast<int>(height));
        ui->width = width;
        ui->height = height;
    }
    if (ui->surface == nullptr)
    {
        ui->device->unlock();
        return -3;
    }
    auto* canvas = ui->surface->getCanvas();
    canvas->clear(tgfx::Color::Transparent());
    for (std::uint32_t index = 0; index < rect_count; ++index)
    {
        const ResonateTgfxRect& rect = rects[index];
        if (rect.width <= 0.0F || rect.height <= 0.0F)
        {
            continue;
        }
        tgfx::Paint paint;
        paint.setColor(tgfx::Color{rect.r, rect.g, rect.b, rect.a});
        canvas->drawRect(tgfx::Rect::MakeXYWH(rect.x, rect.y, rect.width, rect.height), paint);
    }
    for (std::uint32_t index = 0; index < text_count; ++index)
    {
        drawText(canvas, texts[index], ui->typefaces);
    }
    for (std::uint32_t index = 0; index < image_count; ++index)
    {
        const ResonateTgfxImageDraw& draw = images[index];
        const auto found = ui->images.find(draw.id);
        if (found == ui->images.end() || found->second == nullptr || draw.width <= 0.0F ||
            draw.height <= 0.0F)
        {
            continue;
        }
        const std::shared_ptr<tgfx::Image>& image = found->second;
        const tgfx::Rect source = tgfx::Rect::MakeXYWH(
            draw.u0 * static_cast<float>(image->width()), draw.v0 * static_cast<float>(image->height()),
            (draw.u1 - draw.u0) * static_cast<float>(image->width()),
            (draw.v1 - draw.v0) * static_cast<float>(image->height()));
        const tgfx::Rect destination = tgfx::Rect::MakeXYWH(draw.x, draw.y, draw.width, draw.height);
        /* The registered file has no mip chain; the default MipmapMode::Linear
           would sample nothing. */
        const tgfx::SamplingOptions sampling(tgfx::FilterMode::Linear, tgfx::MipmapMode::None);
        canvas->drawImageRect(image, source, destination, sampling);
    }
    context->flushAndSubmit(false);
    if (ui->attached)
    {
        /* The renderer's sampling barrier starts from the layout tgfx left the
           wrapped target in, not from an assumption about it. */
        tgfx::VulkanImageInfo info{};
        if (ui->surface->getBackendTexture().getVulkanImageInfo(&info))
        {
            ui->target_layout = info.layout;
        }
    }
    ui->device->unlock();
    return 0;
}

RESONATE_TGFX_API int32_t resonate_tgfx_image_register(ResonateTgfxUi handle, std::uint32_t id,
                                                       const char* path)
{
    auto* ui = static_cast<Ui*>(handle);
    if (ui == nullptr || path == nullptr)
    {
        return -1;
    }
    /* Decode to CPU pixels instead of Image::MakeFromFile: on Android the file
       decoder hands back an AHardwareBuffer, and binding one as a Vulkan
       texture needs VK_ANDROID_external_memory_android_hardware_buffer, which
       the engine's shared device does not enable. The upload then fails and the
       image never draws. */
    auto codec = tgfx::ImageCodec::MakeFrom(std::string(path));
    if (codec == nullptr || codec->width() <= 0 || codec->height() <= 0)
    {
        return -2;
    }
    const tgfx::ImageInfo info = tgfx::ImageInfo::Make(codec->width(), codec->height(),
                                                       tgfx::ColorType::RGBA_8888,
                                                       tgfx::AlphaType::Premultiplied);
    std::vector<std::uint8_t> pixels(info.byteSize());
    if (!codec->readPixels(info, pixels.data()))
    {
        return -2;
    }
    auto image =
        tgfx::Image::MakeFrom(info, tgfx::Data::MakeWithCopy(pixels.data(), pixels.size()));
    if (image == nullptr || image->width() <= 0 || image->height() <= 0)
    {
        return -2;
    }
    ui->images[id] = std::move(image);
    return 0;
}

RESONATE_TGFX_API void resonate_tgfx_image_release(ResonateTgfxUi handle, std::uint32_t id)
{
    auto* ui = static_cast<Ui*>(handle);
    if (ui == nullptr)
    {
        return;
    }
    ui->images.erase(id);
}

RESONATE_TGFX_API int32_t resonate_tgfx_ui_read_pixels(ResonateTgfxUi handle, void* data,
                                                       std::uint64_t size_bytes)
{
    auto* ui = static_cast<Ui*>(handle);
    if (ui == nullptr || data == nullptr || ui->surface == nullptr)
    {
        return -1;
    }
    const std::uint64_t needed = static_cast<std::uint64_t>(ui->width) * ui->height * 4U;
    if (size_bytes < needed)
    {
        return -1;
    }
    auto* context = ui->device->lockContext();
    if (context == nullptr)
    {
        return -4;
    }
    const tgfx::ImageInfo info =
        tgfx::ImageInfo::Make(static_cast<int>(ui->width), static_cast<int>(ui->height),
                              tgfx::ColorType::RGBA_8888, tgfx::AlphaType::Premultiplied);
    const bool read = ui->surface->readPixels(info, data);
    ui->device->unlock();
    return read ? 0 : -4;
}

RESONATE_TGFX_API int32_t resonate_tgfx_ui_attach(ResonateTgfxUi handle,
                                                  const ResonateTgfxNativeImage* target)
{
    auto* ui = static_cast<Ui*>(handle);
    if (ui == nullptr || target == nullptr || target->image == 0U || target->width == 0U ||
        target->height == 0U)
    {
        return -1;
    }
    auto* context = ui->device->lockContext();
    if (context == nullptr)
    {
        return -2;
    }
    tgfx::VulkanImageInfo info{};
    info.image = target->image;
    info.format = target->format;
    info.layout = target->layout;
    const tgfx::BackendTexture texture(info, static_cast<int>(target->width),
                                       static_cast<int>(target->height));
    auto surface =
        tgfx::Surface::MakeFrom(context, texture, tgfx::ImageOrigin::TopLeft);
    ui->device->unlock();
    if (surface == nullptr)
    {
        return -3;
    }
    ui->surface = std::move(surface);
    ui->width = target->width;
    ui->height = target->height;
    ui->target_layout = target->layout;
    ui->attached = true;
    return 0;
}

RESONATE_TGFX_API std::uint32_t resonate_tgfx_ui_target_layout(ResonateTgfxUi handle)
{
    auto* ui = static_cast<Ui*>(handle);
    return ui != nullptr ? ui->target_layout : 0U;
}

RESONATE_TGFX_API void resonate_tgfx_ui_destroy(ResonateTgfxUi handle)
{
    auto* ui = static_cast<Ui*>(handle);
    if (ui == nullptr)
    {
        return;
    }
    ui->surface.reset();
    ui->typefaces.clear();
    ui->device.reset();
    delete ui;
}

} // extern "C"
