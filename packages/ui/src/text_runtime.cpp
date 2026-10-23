#include <resonate/ui/text_runtime.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include <resonate/ecs/query.h>

#include "glyph_run.h"
#include "resonate.ui/components.gen.h"

namespace resonate::ui
{
namespace
{

float authoredWidth(const RectTransform* rect)
{
    return rect != nullptr && rect->width > 0.0F ? rect->width : 0.0F;
}

} // namespace

struct TextRuntime::Impl
{
    std::vector<std::shared_ptr<const text::Font>> fonts;
    detail::TextAtlasBuilder atlas;
    ecs::Entity canvas{};
    std::uint32_t revision = 1U;
    ecs::World* bound_world = nullptr;
    ecs::Query glyphs;
    bool query_ready = false;

    explicit Impl(std::vector<std::shared_ptr<const text::Font>> input)
        : fonts(std::move(input)), atlas(fonts)
    {
    }
};

TextRuntime::TextRuntime(std::vector<std::shared_ptr<const text::Font>> fonts)
    : impl_(std::make_unique<Impl>(std::move(fonts)))
{
}

TextRuntime::~TextRuntime() = default;

detail::TextAtlasBuilder* TextRuntimeAccess::atlas(TextRuntime& runtime)
{
    return &runtime.impl_->atlas;
}

void TextRuntimeAccess::attach(TextRuntime& runtime, ecs::Entity canvas)
{
    runtime.impl_->canvas = canvas;
    runtime.impl_->revision = 1U;
    runtime.impl_->query_ready = false;
    runtime.impl_->bound_world = nullptr;
}

std::uint32_t TextRuntimeAccess::revision(const TextRuntime& runtime)
{
    return runtime.impl_->revision;
}

bool TextRuntime::setText(ecs::World& world, ecs::Entity canvas, ecs::Entity entity,
                          std::string_view utf8, std::string& error)
{
    if (canvas != impl_->canvas)
    {
        error = "the runtime belongs to another canvas";
        return false;
    }
    Text* text = world.get<Text>(entity);
    if (text == nullptr)
    {
        error = "the entity has no Text";
        return false;
    }
    const RectTransform* rect = world.get<RectTransform>(entity);
    const float width = authoredWidth(rect);
    std::vector<detail::GlyphQuad> quads;
    if (!impl_->atlas.buildRun(utf8, text->font_size, static_cast<float>(text->font_weight), width,
                               static_cast<std::uint32_t>(text->align), text->wrap, quads, error))
    {
        return false;
    }

    const ecs::BlobHandle old_content = text->content;
    ecs::BlobHandle content{};
    if (!utf8.empty())
    {
        content = world.createBlob(entity, utf8.size(), utf8.data());
        if (!content.valid())
        {
            error = "the text bytes could not be attached";
            return false;
        }
    }
    text = world.get<Text>(entity);
    text->content = content;
    if (old_content.valid())
    {
        world.destroyBlob(old_content);
    }

    if (Glyphs* glyphs = world.get<Glyphs>(entity))
    {
        if (world.alive(glyphs->records))
        {
            world.destroyBlob(glyphs->records);
        }
        world.remove<Glyphs>(entity);
    }
    for (detail::GlyphQuad& quad : quads)
    {
        if (!impl_->atlas.updateQuad(quad))
        {
            error = "a glyph is missing from the atlas";
            return false;
        }
    }
    if (!quads.empty())
    {
        const ecs::BlobHandle records =
            world.createBlob(entity, quads.size() * sizeof(detail::GlyphQuad), quads.data());
        if (!records.valid() || world.add(entity, Glyphs{records}) == nullptr)
        {
            error = "glyph quads could not be attached";
            return false;
        }
    }

    float minimum_x = 0.0F;
    float minimum_y = 0.0F;
    float maximum_x = 0.0F;
    float maximum_y = 0.0F;
    if (!quads.empty())
    {
        minimum_x = quads[0].x;
        minimum_y = quads[0].y;
        maximum_x = quads[0].x + quads[0].width;
        maximum_y = quads[0].y + quads[0].height;
        for (const detail::GlyphQuad& quad : quads)
        {
            minimum_x = std::fmin(minimum_x, quad.x);
            minimum_y = std::fmin(minimum_y, quad.y);
            maximum_x = std::fmax(maximum_x, quad.x + quad.width);
            maximum_y = std::fmax(maximum_y, quad.y + quad.height);
        }
    }
    if (TextMetrics* metrics = world.get<TextMetrics>(entity))
    {
        metrics->width = maximum_x - minimum_x;
        metrics->height = maximum_y - minimum_y;
    }
    else if (world.add(entity, TextMetrics{maximum_x - minimum_x, maximum_y - minimum_y}) ==
             nullptr)
    {
        error = "text metrics could not be attached";
        return false;
    }

    /* A grown page moved every cached glyph, so every run refreshes its UVs. */
    if (!impl_->query_ready || impl_->bound_world != &world)
    {
        const ecs::ComponentIndex all[] = {ecs::ComponentTraits<Glyphs>::index};
        impl_->glyphs = world.createQuery(ecs::QueryDesc{all, {}, {}});
        impl_->query_ready = impl_->glyphs.valid();
        impl_->bound_world = &world;
        if (!impl_->query_ready)
        {
            error = "the glyph query could not be created";
            return false;
        }
    }
    struct Refresh
    {
        ecs::World* world = nullptr;
        detail::TextAtlasBuilder* atlas = nullptr;
    };
    Refresh refresh{&world, &impl_->atlas};
    impl_->glyphs.forEachChunk(
        [](void* context, ecs::ChunkView view)
        {
            auto* state = static_cast<Refresh*>(context);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                Glyphs* glyphs = state->world->get<Glyphs>(view.entities[row]);
                if (glyphs == nullptr || !state->world->alive(glyphs->records))
                {
                    continue;
                }
                auto* records =
                    static_cast<detail::GlyphQuad*>(state->world->blobData(glyphs->records));
                const std::size_t count =
                    state->world->blobSize(glyphs->records) / sizeof(detail::GlyphQuad);
                for (std::size_t index = 0; index < count; ++index)
                {
                    state->atlas->updateQuad(records[index]);
                }
            }
        },
        &refresh);

    TextAtlasPage* page = world.get<TextAtlasPage>(canvas);
    ++impl_->revision;
    if (impl_->revision == 0U)
    {
        ++impl_->revision;
    }
    const std::uint32_t revision = impl_->revision;
    if (page == nullptr)
    {
        const ecs::BlobHandle pixels =
            world.createBlob(canvas, impl_->atlas.pixels().size(), impl_->atlas.pixels().data());
        if (!pixels.valid() ||
            world.add(canvas, TextAtlasPage{impl_->atlas.width(), impl_->atlas.height(), revision,
                                            pixels}) == nullptr)
        {
            error = "the font atlas page could not be attached";
            return false;
        }
    }
    else
    {
        if (!world.alive(page->pixels) ||
            world.blobSize(page->pixels) != impl_->atlas.pixels().size())
        {
            const ecs::BlobHandle old = page->pixels;
            const ecs::BlobHandle pixels = world.createBlob(canvas, impl_->atlas.pixels().size(),
                                                            impl_->atlas.pixels().data());
            if (!pixels.valid())
            {
                error = "the font atlas page could not grow";
                return false;
            }
            page = world.get<TextAtlasPage>(canvas);
            page->pixels = pixels;
            if (old.valid())
            {
                world.destroyBlob(old);
            }
        }
        else
        {
            std::memcpy(world.blobData(page->pixels), impl_->atlas.pixels().data(),
                        impl_->atlas.pixels().size());
        }
        page = world.get<TextAtlasPage>(canvas);
        page->width = impl_->atlas.width();
        page->height = impl_->atlas.height();
        page->revision = revision;
    }
    return true;
}

} // namespace resonate::ui
