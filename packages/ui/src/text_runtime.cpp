#include <resonate/ui/text_runtime.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include <resonate/ecs/query.h>
#include <resonate/hierarchy/tree.h>

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

/* The selected byte range, empty when no anchor or the anchor equals the
   caret. The caret is the moving edge; both clamp to the content. */
void selectionRangeOf(ecs::World& world, ecs::Entity entity, std::size_t content_size,
                      std::size_t& out_begin, std::size_t& out_end)
{
    const TextSelection* selection = world.get<TextSelection>(entity);
    const std::size_t caret = caretOffset(world, entity, content_size);
    /* No anchor component means no selection: the anchor sits on the caret,
       not at the content's end, or every edit would read one. */
    const std::size_t anchor =
        selection != nullptr && selection->anchor <= content_size ? selection->anchor : caret;
    out_begin = anchor < caret ? anchor : caret;
    out_end = anchor < caret ? caret : anchor;
}

void clearSelection(ecs::World& world, ecs::Entity entity)
{
    if (world.get<TextSelection>(entity) != nullptr)
    {
        world.remove<TextSelection>(entity);
    }
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
    if (!focused.valid() || !isTextField(world, focused))
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
    /* A wholesale replacement is not a caret edit: whatever range pointed into
       the old bytes is stale, so it dies with them. */
    clearSelection(world, entity);
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
    float ascent = text->font_size * 0.8F;
    float descent = -text->font_size * 0.2F;
    impl_->atlas.lineMetrics(text->font_size, static_cast<float>(text->font_weight), ascent,
                             descent);
    if (TextMetrics* metrics = world.get<TextMetrics>(entity))
    {
        metrics->width = maximum_x - minimum_x;
        metrics->height = maximum_y - minimum_y;
        metrics->advance = advance;
        metrics->ascent = ascent;
        metrics->descent = descent;
    }
    else if (world.add(entity, TextMetrics{maximum_x - minimum_x, maximum_y - minimum_y, advance,
                                           ascent, descent}) == nullptr)
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
    if (!isTextField(world, pointer->focused))
    {
        return true;
    }
    std::string content = readContent(world, pointer->focused);
    std::size_t caret = caretOffset(world, pointer->focused, content.size());
    std::size_t select_begin = 0U;
    std::size_t select_end = 0U;
    selectionRangeOf(world, pointer->focused, content.size(), select_begin, select_end);
    /* A selection is the edit: either key deletes it whole, and typed text
       replaces it. An empty range is no edit, but its anchor still dies: left
       behind, the next edit past it would read as a selection. */
    if (select_begin < select_end)
    {
        content.erase(select_begin, select_end - select_begin);
        caret = select_begin;
        backspace = false;
        delete_forward = false;
    }
    clearSelection(world, pointer->focused);
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

namespace
{

/* The byte offset a press or drag edge lands on: before the glyph whose cell
   it is in, split at the cell's middle. */
std::size_t caretAtLocal(ecs::World& world, ecs::Entity entity, float local_x)
{
    local_x -= world.get<Text>(entity)->padding_left;
    const Glyphs* glyphs = world.get<Glyphs>(entity);
    if (glyphs == nullptr || !world.alive(glyphs->records))
    {
        return 0U;
    }
    const auto* records = static_cast<const detail::GlyphQuad*>(world.blobData(glyphs->records));
    const std::size_t count = world.blobSize(glyphs->records) / sizeof(detail::GlyphQuad);
    const TextMetrics* metrics = world.get<TextMetrics>(entity);
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
            return records[index].byte_offset;
        }
    }
    return readContent(world, entity).size();
}

} // namespace

bool placeCaretAt(ecs::World& world, ecs::Entity entity, float local_x)
{
    if (world.get<Text>(entity) == nullptr)
    {
        return false;
    }
    clearSelection(world, entity);
    return setCaret(world, entity, caretAtLocal(world, entity, local_x));
}

bool beginTextSelect(ecs::World& world, ecs::Entity entity, float local_x)
{
    if (world.get<Text>(entity) == nullptr)
    {
        return false;
    }
    clearSelection(world, entity);
    if (!setCaret(world, entity, caretAtLocal(world, entity, local_x)))
    {
        return false;
    }
    /* The anchor starts at the caret; until a drag moves the edge, the range
       is empty and the field behaves as if it had none. */
    const TextCaret* caret = world.get<TextCaret>(entity);
    return world.add(entity, TextSelection{caret != nullptr ? caret->offset : 0U}) != nullptr;
}

bool extendCaretTo(ecs::World& world, ecs::Entity entity, float local_x)
{
    if (world.get<Text>(entity) == nullptr)
    {
        return false;
    }
    if (world.get<TextSelection>(entity) == nullptr &&
        world.add(entity, TextSelection{static_cast<std::uint32_t>(
                              caretOffset(world, entity, readContent(world, entity).size()))}) ==
            nullptr)
    {
        return false;
    }
    return setCaret(world, entity, caretAtLocal(world, entity, local_x));
}

bool selectionRange(ecs::World& world, ecs::Entity entity, std::uint32_t& out_begin,
                    std::uint32_t& out_end)
{
    const Text* text = world.get<Text>(entity);
    if (text == nullptr)
    {
        return false;
    }
    std::size_t begin = 0U;
    std::size_t end = 0U;
    selectionRangeOf(world, entity, readContent(world, entity).size(), begin, end);
    out_begin = static_cast<std::uint32_t>(begin);
    out_end = static_cast<std::uint32_t>(end);
    return true;
}

bool moveCaret(ecs::World& world, ecs::Entity canvas, CaretMotion motion)
{
    const PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr || !pointer->focused.valid() ||
        !isTextField(world, pointer->focused))
    {
        return false;
    }
    const std::string content = readContent(world, pointer->focused);
    const std::size_t caret = caretOffset(world, pointer->focused, content.size());
    std::size_t moved = caret;
    std::size_t select_begin = 0U;
    std::size_t select_end = 0U;
    selectionRangeOf(world, pointer->focused, content.size(), select_begin, select_end);
    if (select_begin < select_end)
    {
        /* A selection collapses at the edge the motion points at; a second
           press of the same key then moves from there. */
        switch (motion)
        {
            case CaretMotion::Left:
                moved = select_begin;
                break;
            case CaretMotion::Right:
                moved = select_end;
                break;
            case CaretMotion::Home:
                moved = 0U;
                break;
            case CaretMotion::End:
                moved = content.size();
                break;
        }
    }
    else
    {
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
    }
    clearSelection(world, pointer->focused);
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
        !isTextField(world, pointer->focused))
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

bool setText(ecs::World& world, ecs::Entity canvas, ecs::Entity entity, std::string_view utf8,
             std::string& error)
{
    const TextRuntimeRef* reference = world.get<TextRuntimeRef>(canvas);
    if (reference == nullptr || reference->runtime == nullptr)
    {
        error = "the canvas carries no text runtime";
        return false;
    }
    return reference->runtime->setText(world, canvas, entity, utf8, error);
}

namespace
{

/* The caption a row shows: its own Text, or the first Text under it. */
ecs::Entity captionOf(ecs::World& world, ecs::Entity row)
{
    if (world.get<Text>(row) != nullptr)
    {
        return row;
    }
    ecs::Entity found{};
    for (const ecs::Entity child : hierarchy::children(world, row))
    {
        found = captionOf(world, child);
        if (found.valid())
        {
            break;
        }
    }
    return found;
}

bool syncDropdown(ecs::World& world, ecs::Entity canvas, TextRuntime& runtime, ecs::Entity entity,
                  std::string& error)
{
    const Dropdown* dropdown = world.get<Dropdown>(entity);
    if (dropdown == nullptr)
    {
        return true;
    }
    for (const ecs::Entity child : hierarchy::children(world, entity))
    {
        if (world.get<DropdownList>(child) == nullptr)
        {
            continue;
        }
        const auto rows = hierarchy::children(world, child);
        if (dropdown->selected >= rows.size())
        {
            return true;
        }
        const ecs::Entity caption = captionOf(world, rows[dropdown->selected]);
        if (!caption.valid() || caption == entity)
        {
            return true;
        }
        const std::string selected = readContent(world, caption);
        if (readContent(world, entity) == selected)
        {
            return true;
        }
        return runtime.setText(world, canvas, entity, selected, error);
    }
    return true;
}

bool syncDropdownsIn(ecs::World& world, ecs::Entity canvas, TextRuntime& runtime,
                     ecs::Entity entity, std::string& error)
{
    if (!syncDropdown(world, canvas, runtime, entity, error))
    {
        return false;
    }
    for (const ecs::Entity child : hierarchy::children(world, entity))
    {
        if (!syncDropdownsIn(world, canvas, runtime, child, error))
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool syncDropdowns(ecs::World& world, ecs::Entity canvas, std::string& error)
{
    const TextRuntimeRef* reference = world.get<TextRuntimeRef>(canvas);
    if (reference == nullptr || reference->runtime == nullptr)
    {
        return true;
    }
    return syncDropdownsIn(world, canvas, *reference->runtime, canvas, error);
}

bool isTextField(ecs::World& world, ecs::Entity entity)
{
    if (world.get<Text>(entity) == nullptr || world.get<Input>(entity) == nullptr)
    {
        return false;
    }
    /* Text on any of these is a caption over an interaction the component
       already owns; a caret there is a stray cursor over a checkbox. */
    return world.get<Checkbox>(entity) == nullptr && world.get<Slider>(entity) == nullptr &&
           world.get<Dropdown>(entity) == nullptr && world.get<DropdownList>(entity) == nullptr &&
           world.get<Scrubber>(entity) == nullptr && world.get<Selectable>(entity) == nullptr &&
           world.get<Tab>(entity) == nullptr && world.get<Tabs>(entity) == nullptr &&
           world.get<TabPage>(entity) == nullptr && world.get<Splitter>(entity) == nullptr;
}

bool focusedTextEditable(ecs::World& world, ecs::Entity canvas)
{
    const PointerState* pointer = world.get<PointerState>(canvas);
    return pointer != nullptr && pointer->focused.valid() && isTextField(world, pointer->focused);
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
        !isTextField(world, pointer->focused))
    {
        return {};
    }
    return readContent(world, pointer->focused);
}

} // namespace resonate::ui
