#include <resonate/ui/text_runtime.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>
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

std::string readContent(ecs::World& world, ecs::Entity entity)
{
    std::string content;
    const Text* text = world.get<Text>(entity);
    if (text != nullptr && world.alive(text->content))
    {
        const auto* bytes = static_cast<const char*>(world.blobData(text->content));
        if (bytes != nullptr)
        {
            content.assign(bytes, world.blobSize(text->content));
        }
    }
    return content;
}

/* The caret's byte offset; the content's end when the field carries none. */
std::size_t caretOffset(ecs::World& world, ecs::Entity entity, std::size_t content_size)
{
    const TextCaret* caret = world.get<TextCaret>(entity);
    return caret != nullptr && caret->offset < content_size ? caret->offset : content_size;
}

bool setCaret(ecs::World& world, ecs::Entity entity, std::size_t offset)
{
    TextCaret* caret = world.get<TextCaret>(entity);
    if (caret == nullptr)
    {
        return world.add(entity, TextCaret{static_cast<std::uint32_t>(offset)}) != nullptr;
    }
    if (caret->offset != offset)
    {
        caret->offset = static_cast<std::uint32_t>(offset);
        world.markChanged<TextCaret>(entity);
    }
    return true;
}

/* One code point left: UTF-8 continuation bytes step back to their lead. */
std::size_t previousCodepoint(const std::string& content, std::size_t offset)
{
    if (offset == 0U)
    {
        return 0U;
    }
    std::size_t index = offset - 1U;
    while (index > 0U && (static_cast<unsigned char>(content[index]) & 0xC0U) == 0x80U)
    {
        --index;
    }
    return index;
}

/* One code point right; the end when already there. */
std::size_t nextCodepoint(const std::string& content, std::size_t offset)
{
    if (offset >= content.size())
    {
        return content.size();
    }
    std::size_t index = offset + 1U;
    while (index < content.size() && (static_cast<unsigned char>(content[index]) & 0xC0U) == 0x80U)
    {
        ++index;
    }
    return index;
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
    /* The field showing a preedit, the committed text to restore and the
       preedit currently drawn, so an unchanged poll does no work. */
    ecs::Entity composition_entity{};
    std::string composition_base;
    std::string composition_preedit;

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
    runtime.impl_->composition_entity = ecs::Entity{};
    runtime.impl_->composition_base.clear();
    runtime.impl_->composition_preedit.clear();
}

std::uint32_t TextRuntimeAccess::revision(const TextRuntime& runtime)
{
    return runtime.impl_->revision;
}

bool TextRuntimeAccess::restoreComposition(ecs::World& world, ecs::Entity keep,
                                           TextRuntime& runtime, std::string& error)
{
    TextRuntime::Impl& impl = *runtime.impl_;
    if (!impl.composition_entity.valid() || impl.composition_entity == keep)
    {
        return true;
    }
    const ecs::Entity entity = impl.composition_entity;
    const std::string base = std::move(impl.composition_base);
    impl.composition_entity = ecs::Entity{};
    impl.composition_base.clear();
    impl.composition_preedit.clear();
    if (!world.alive(entity) || world.get<Text>(entity) == nullptr)
    {
        return true;
    }
    return runtime.setText(world, impl.canvas, entity, base, error);
}

bool TextRuntimeAccess::updateComposition(ecs::World& world, ecs::Entity canvas,
                                          TextRuntime& runtime, std::string_view utf8,
                                          std::string& error)
{
    TextRuntime::Impl& impl = *runtime.impl_;
    if (canvas != impl.canvas)
    {
        error = "the runtime belongs to another canvas";
        return false;
    }
    const PointerState* pointer = world.get<PointerState>(canvas);
    const ecs::Entity focused = pointer != nullptr ? pointer->focused : ecs::Entity{};
    if (utf8.empty())
    {
        return restoreComposition(world, ecs::Entity{}, runtime, error);
    }
    if (!restoreComposition(world, focused, runtime, error))
    {
        return false;
    }
    if (!focused.valid() || world.get<Text>(focused) == nullptr)
    {
        return true;
    }
    if (impl.composition_entity == focused && impl.composition_preedit == utf8)
    {
        return true;
    }
    if (impl.composition_entity != focused)
    {
        impl.composition_base = readContent(world, focused);
        impl.composition_entity = focused;
    }
    impl.composition_preedit.assign(utf8);
    std::string preview = impl.composition_base;
    preview.append(utf8);
    if (!runtime.setText(world, canvas, focused, preview, error))
    {
        return false;
    }
    if (!setCaret(world, focused, preview.size()))
    {
        error = "the caret could not be attached";
        return false;
    }
    return true;
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
    if (TextCaret* caret = world.get<TextCaret>(entity);
        caret != nullptr && caret->offset > utf8.size())
    {
        caret->offset = static_cast<std::uint32_t>(utf8.size());
        world.markChanged<TextCaret>(entity);
    }
    const RectTransform* rect = world.get<RectTransform>(entity);
    const float width = std::fmax(0.0F, authoredWidth(rect) - text->padding_left);
    std::vector<detail::GlyphQuad> quads;
    float advance = 0.0F;
    if (!impl_->atlas.buildRun(utf8, text->font_size, static_cast<float>(text->font_weight), width,
                               static_cast<std::uint32_t>(text->align), text->wrap, quads, advance,
                               error))
    {
        return false;
    }
    /* Refresh the UVs before touching the entity, so a failed refresh leaves
       the previous text intact. */
    for (detail::GlyphQuad& quad : quads)
    {
        if (!impl_->atlas.updateQuad(quad))
        {
            error = "a glyph is missing from the atlas";
            return false;
        }
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
        metrics->advance = advance;
    }
    else if (world.add(entity, TextMetrics{maximum_x - minimum_x, maximum_y - minimum_y,
                                           advance}) == nullptr)
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

bool updateTextInput(ecs::World& world, ecs::Entity canvas, TextRuntime& runtime,
                     std::string_view utf8, bool backspace, bool delete_forward,
                     std::string& error)
{
    if (!TextRuntimeAccess::restoreComposition(world, ecs::Entity{}, runtime, error))
    {
        return false;
    }
    const PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr || !pointer->focused.valid())
    {
        return true;
    }
    if (world.get<Text>(pointer->focused) == nullptr)
    {
        return true;
    }

    std::string content = readContent(world, pointer->focused);
    std::size_t caret = caretOffset(world, pointer->focused, content.size());
    if (backspace && caret > 0U)
    {
        const std::size_t start = previousCodepoint(content, caret);
        content.erase(start, caret - start);
        caret = start;
    }
    else if (delete_forward && caret < content.size())
    {
        content.erase(caret, nextCodepoint(content, caret) - caret);
    }
    /* Some platforms hand control keys back as text input — Win32 translates
       the backspace key into a WM_CHAR the IME layer forwards. Text is
       printable characters; a stored control byte would be invisible and
       swallow the next deletion. */
    std::string typed;
    for (const char byte : utf8)
    {
        const auto value = static_cast<unsigned char>(byte);
        if (value >= 0x20U && value != 0x7FU)
        {
            typed.push_back(byte);
        }
    }
    if (!typed.empty())
    {
        content.insert(caret, typed);
        caret += typed.size();
    }
    if (!setCaret(world, pointer->focused, caret))
    {
        error = "the caret could not be attached";
        return false;
    }
    return runtime.setText(world, canvas, pointer->focused, content, error);
}

bool placeCaretAt(ecs::World& world, ecs::Entity entity, float local_x)
{
    const Text* text = world.get<Text>(entity);
    if (text == nullptr)
    {
        return false;
    }
    local_x -= text->padding_left;
    const Glyphs* glyphs = world.get<Glyphs>(entity);
    if (glyphs == nullptr || !world.alive(glyphs->records))
    {
        /* An empty field takes the caret at the only place it can be. */
        return setCaret(world, entity, 0U);
    }
    const auto* records = static_cast<const detail::GlyphQuad*>(world.blobData(glyphs->records));
    const std::size_t count = world.blobSize(glyphs->records) / sizeof(detail::GlyphQuad);
    const TextMetrics* metrics = world.get<TextMetrics>(entity);
    /* A click lands before the glyph whose cell it is in, split at the cell's
       middle; the cell ends where the next glyph starts. */
    for (std::size_t index = 0; index < count; ++index)
    {
        const float start = records[index].pen_x;
        const float end = index + 1U < count
                              ? records[index + 1U].pen_x
                              : (metrics != nullptr && metrics->advance > start
                                     ? metrics->advance
                                     : start + 1.0F);
        if (local_x < (start + end) * 0.5F)
        {
            return setCaret(world, entity, records[index].byte_offset);
        }
    }
    return setCaret(world, entity, readContent(world, entity).size());
}

bool moveCaret(ecs::World& world, ecs::Entity canvas, CaretMotion motion)
{
    const PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr || !pointer->focused.valid() ||
        world.get<Text>(pointer->focused) == nullptr)
    {
        return false;
    }
    const std::string content = readContent(world, pointer->focused);
    const std::size_t caret = caretOffset(world, pointer->focused, content.size());
    std::size_t moved = caret;
    switch (motion)
    {
        case CaretMotion::Left:
            moved = previousCodepoint(content, caret);
            break;
        case CaretMotion::Right:
            moved = nextCodepoint(content, caret);
            break;
        case CaretMotion::Home:
            moved = 0U;
            break;
        case CaretMotion::End:
            moved = content.size();
            break;
    }
    return setCaret(world, pointer->focused, moved);
}

bool updateTextComposition(ecs::World& world, ecs::Entity canvas, TextRuntime& runtime,
                           std::string_view utf8, std::string& error)
{
    return TextRuntimeAccess::updateComposition(world, canvas, runtime, utf8, error);
}

bool setFocusedText(ecs::World& world, ecs::Entity canvas, TextRuntime& runtime,
                    std::string_view utf8, std::string& error)
{
    const PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr || !pointer->focused.valid() ||
        world.get<Text>(pointer->focused) == nullptr)
    {
        return true;
    }
    if (readContent(world, pointer->focused) == utf8)
    {
        return setCaret(world, pointer->focused, utf8.size());
    }
    if (!runtime.setText(world, canvas, pointer->focused, std::string(utf8), error))
    {
        return false;
    }
    return setCaret(world, pointer->focused, utf8.size());
}

bool focusedTextEditable(ecs::World& world, ecs::Entity canvas)
{
    const PointerState* pointer = world.get<PointerState>(canvas);
    return pointer != nullptr && pointer->focused.valid() &&
           world.get<Text>(pointer->focused) != nullptr;
}

ecs::Entity focusedEntity(ecs::World& world, ecs::Entity canvas)
{
    const PointerState* pointer = world.get<PointerState>(canvas);
    return pointer != nullptr ? pointer->focused : ecs::Entity{};
}

std::string focusedText(ecs::World& world, ecs::Entity canvas)
{
    const PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr || !pointer->focused.valid() ||
        world.get<Text>(pointer->focused) == nullptr)
    {
        return {};
    }
    return readContent(world, pointer->focused);
}

} // namespace resonate::ui
