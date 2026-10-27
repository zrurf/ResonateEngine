#include <resonate/ui/display_list.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#include <resonate/hierarchy/tree.h>

#include "glyph_run.h"
#include "widget_shown.h"

#include <resonate/ui/input.h>
#include <resonate/ui/text_runtime.h>
#include "resonate.ui/components.gen.h"

namespace resonate::ui
{
namespace
{

bool report(ecs::World& world, ecs::Entity entity, const char* what)
{
    world.report(ecs::World::Report::Error, "ui display list: entity %u:%u %s", entity.index,
                 entity.generation, what);
    return false;
}

} // namespace

DisplayList::Clip DisplayList::intersect(const Clip& outer, float x, float y, float width,
                                         float height)
{
    Clip clip;
    clip.active = true;
    clip.left = outer.active ? std::max(outer.left, x) : x;
    clip.top = outer.active ? std::max(outer.top, y) : y;
    clip.right = outer.active ? std::min(outer.right, x + width) : x + width;
    clip.bottom = outer.active ? std::min(outer.bottom, y + height) : y + height;
    return clip;
}

bool DisplayList::visible(const Clip& clip, float x, float y, float width, float height)
{
    if (!clip.active)
    {
        return true;
    }
    return x < clip.right && x + width > clip.left && y < clip.bottom && y + height > clip.top;
}

bool DisplayList::paint(ecs::World& world, ecs::Entity entity, std::uint32_t depth,
                        const Clip& outer)
{
    if (depth > hierarchy::kMaxDepth)
    {
        return report(world, entity, "sits below the UI tree's depth limit");
    }
    /* Clips travel with every draw so a rasterizer can confine it. */
    const auto clip_of = [](const Clip& clip) -> ClipRect
    {
        if (!clip.active)
        {
            return ClipRect{};
        }
        return ClipRect{clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top};
    };
    const std::size_t quad_begin = quads_.size();

    Clip clip = outer;
    if (const ScrollView* view = world.get<ScrollView>(entity))
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has ScrollView but no ComputedRect");
        }
        clip = intersect(outer, rect->x, rect->y, rect->width, rect->height);
        /* The thumb geometry is shared with the input system, which drags it. */
        const detail::ScrollThumb thumb = detail::scrollThumbOf(*rect, *view);
        if (thumb.visible && visible(clip, thumb.x, thumb.y, thumb.width, thumb.height))
        {
            quads_.push_back(Quad{thumb.x, thumb.y, thumb.width, thumb.height, view->color});
        }
    }

    if (const Background* background = world.get<Background>(entity))
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has Background but no ComputedRect");
        }
        if (background->color.w > 0.0F &&
            visible(clip, rect->x, rect->y, rect->width, rect->height))
        {
            quads_.push_back(Quad{rect->x, rect->y, rect->width, rect->height, background->color});
        }
        /* The outline is drawn inside the rect: top and bottom strips span the
           full width, the side strips fill between them so no pixel blends
           twice. */
        if (background->border_width > 0.0F && background->border_color.w > 0.0F)
        {
            const float width = std::fmin(background->border_width,
                                          std::fmin(rect->width, rect->height) * 0.5F);
            if (width > 0.0F)
            {
                quads_.push_back(
                    Quad{rect->x, rect->y, rect->width, width, background->border_color});
                quads_.push_back(Quad{rect->x, rect->y + rect->height - width, rect->width, width,
                                      background->border_color});
                quads_.push_back(Quad{rect->x, rect->y + width, width, rect->height - 2.0F * width,
                                      background->border_color});
                quads_.push_back(Quad{rect->x + rect->width - width, rect->y + width, width,
                                      rect->height - 2.0F * width, background->border_color});
            }
        }
    }
    if (const Image* image = world.get<Image>(entity))
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has Image but no ComputedRect");
        }
        float left = std::clamp(image->slice.x, 0.0F, 1.0F);
        float top = std::clamp(image->slice.y, 0.0F, 1.0F);
        float right = std::clamp(image->slice.z, 0.0F, 1.0F);
        float bottom = std::clamp(image->slice.w, 0.0F, 1.0F);
        if (left + right > 1.0F)
        {
            const float sum = left + right;
            left /= sum;
            right /= sum;
        }
        if (top + bottom > 1.0F)
        {
            const float sum = top + bottom;
            top /= sum;
            bottom /= sum;
        }
        if (left + right <= 0.0F && top + bottom <= 0.0F)
        {
            if (visible(clip, rect->x, rect->y, rect->width, rect->height))
            {
                quads_.push_back(Quad{rect->x, rect->y, rect->width, rect->height, image->tint,
                                      image->texture, image->uv_min, image->uv_max});
            }
        }
        else
        {
            /* A nine-slice splits both the quad and its UV rect at the same
               fractions; the corners and edges keep their source region. */
            const float xs[4] = {rect->x, rect->x + rect->width * left,
                                 rect->x + rect->width * (1.0F - right),
                                 rect->x + rect->width};
            const float ys[4] = {rect->y, rect->y + rect->height * top,
                                 rect->y + rect->height * (1.0F - bottom),
                                 rect->y + rect->height};
            const float us[4] = {image->uv_min.x,
                                 image->uv_min.x + (image->uv_max.x - image->uv_min.x) * left,
                                 image->uv_max.x - (image->uv_max.x - image->uv_min.x) * right,
                                 image->uv_max.x};
            const float vs[4] = {image->uv_min.y,
                                 image->uv_min.y + (image->uv_max.y - image->uv_min.y) * top,
                                 image->uv_max.y - (image->uv_max.y - image->uv_min.y) * bottom,
                                 image->uv_max.y};
            for (std::uint32_t row = 0; row < 3U; ++row)
            {
                for (std::uint32_t column = 0; column < 3U; ++column)
                {
                    const float quad_width = xs[column + 1U] - xs[column];
                    const float quad_height = ys[row + 1U] - ys[row];
                    if (quad_width <= 0.0F || quad_height <= 0.0F)
                    {
                        continue;
                    }
                    if (!visible(clip, xs[column], ys[row], quad_width, quad_height))
                    {
                        continue;
                    }
                    quads_.push_back(Quad{xs[column], ys[row], quad_width, quad_height,
                                          image->tint, image->texture,
                                          Vec2{us[column], vs[row]},
                                          Vec2{us[column + 1U], vs[row + 1U]}});
                }
            }
        }
    }
    if (const Selectable* selectable = world.get<Selectable>(entity))
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has Selectable but no ComputedRect");
        }
        if (selectable->selected && selectable->color.w > 0.0F &&
            visible(clip, rect->x, rect->y, rect->width, rect->height))
        {
            quads_.push_back(
                Quad{rect->x, rect->y, rect->width, rect->height, selectable->color});
        }
    }
    if (const Slider* slider = world.get<Slider>(entity))
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has Slider but no ComputedRect");
        }
        const float track_height = 4.0F;
        const float track_y = rect->y + (rect->height - track_height) * 0.5F;
        Vec4 track_color = slider->color;
        track_color.w *= 0.35F;
        float fraction = 0.0F;
        if (slider->max > slider->min)
        {
            fraction = (slider->value - slider->min) / (slider->max - slider->min);
            fraction = std::clamp(fraction, 0.0F, 1.0F);
        }
        if (visible(clip, rect->x, track_y, rect->width, track_height))
        {
            quads_.push_back(Quad{rect->x, track_y, rect->width, track_height, track_color});
            if (fraction > 0.0F)
            {
                quads_.push_back(
                    Quad{rect->x, track_y, rect->width * fraction, track_height, slider->color});
            }
        }
        const float knob_width = 10.0F;
        const float knob_height = 14.0F;
        /* The knob travels within the track; a track narrower than the knob has
           no travel at all (clamping to width - knob would invert the bounds). */
        const float knob_travel = std::fmax(0.0F, rect->width - knob_width);
        const float knob_x = rect->x + knob_travel * fraction;
        if (visible(clip, knob_x, rect->y, knob_width, knob_height))
        {
            quads_.push_back(Quad{knob_x, rect->y + (rect->height - knob_height) * 0.5F,
                                  knob_width, knob_height, slider->color});
        }
    }
    if (const Checkbox* checkbox = world.get<Checkbox>(entity))
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has Checkbox but no ComputedRect");
        }
        /* A 14 px box on the left, vertically centred; a checked box fills an
           inset square. The author leaves room for it with padding_left. */
        const float box = 14.0F;
        const float left = rect->x + 4.0F;
        const float top = rect->y + (rect->height - box) * 0.5F;
        const float edge = 1.0F;
        if (visible(clip, left, top, box, box))
        {
            quads_.push_back(Quad{left, top, box, edge, checkbox->color});
            quads_.push_back(Quad{left, top + box - edge, box, edge, checkbox->color});
            quads_.push_back(Quad{left, top + edge, edge, box - 2.0F * edge, checkbox->color});
            quads_.push_back(
                Quad{left + box - edge, top + edge, edge, box - 2.0F * edge, checkbox->color});
        }
        if (checkbox->checked && visible(clip, left, top, box, box))
        {
            quads_.push_back(Quad{left + 3.0F, top + 3.0F, box - 6.0F, box - 6.0F,
                                  checkbox->color});
        }
    }
    if (const Text* text = world.get<Text>(entity))
    {
        const Glyphs* glyphs = world.get<Glyphs>(entity);
        const bool has_glyphs = glyphs != nullptr && world.alive(glyphs->records);
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            if (has_glyphs || focused_ == entity)
            {
                return report(world, entity, "has Text but no ComputedRect");
            }
        }
        else
        {
            const detail::GlyphQuad* records = nullptr;
            std::size_t count = 0;
            if (has_glyphs)
            {
                records = static_cast<const detail::GlyphQuad*>(world.blobData(glyphs->records));
                count = world.blobSize(glyphs->records) / sizeof(detail::GlyphQuad);
            }
            const bool label_visible =
                visible(clip, rect->x, rect->y, rect->width, rect->height);
            /* The ink is placed inside the rect: horizontally the shaping
               already applied the alignment span (padding_left shrank it), so
               only the left inset shifts it; vertically the ink extents decide
               middle and bottom. */
            /* A focused field whose caret passed the box scrolls its own text
               so the caret stays visible; the clip keeps the rest cut. */
            float caret_local = 0.0F;
            float scroll_x = 0.0F;
            /* The glyph the caret sits before; count is past the last one, whose
               line is where the caret then is. */
            std::size_t caret_index = count;
            const TextCaret* caret = focused_ == entity ? world.get<TextCaret>(entity) : nullptr;
            if (focused_ == entity && count > 0U)
            {
                const TextMetrics* metrics = world.get<TextMetrics>(entity);
                /* Past the last glyph the caret sits where the run ends. */
                caret_local = metrics != nullptr ? metrics->advance : records[count - 1U].pen_x;
                if (caret != nullptr)
                {
                    for (std::size_t index = 0; index < count; ++index)
                    {
                        if (records[index].byte_offset >= caret->offset)
                        {
                            caret_local = records[index].pen_x;
                            caret_index = index;
                            break;
                        }
                    }
                }
                const float visible = rect->width - text->padding_left;
                if (visible > 0.0F && caret_local > visible)
                {
                    scroll_x = caret_local - visible;
                }
            }
            const float origin_x = rect->x + text->padding_left - scroll_x;
            float origin_y = rect->y;
            if (text->v_align != VAlign::Top && count > 0U)
            {
                float minimum_y = records[0].y;
                float maximum_y = records[0].y + records[0].height;
                for (std::size_t index = 1U; index < count; ++index)
                {
                    minimum_y = std::fmin(minimum_y, records[index].y);
                    maximum_y = std::fmax(maximum_y, records[index].y + records[index].height);
                }
                const float ink_height = maximum_y - minimum_y;
                origin_y += text->v_align == VAlign::Middle
                                ? (rect->height - ink_height) * 0.5F - minimum_y
                                : rect->height - ink_height - minimum_y;
            }
            /* A widget clips its own text: a field's overflow stops at its box,
               not at the viewport. */
            const Clip text_clip =
                rect->width > 0.0F && rect->height > 0.0F
                    ? intersect(clip, rect->x, rect->y, rect->width, rect->height)
                    : clip;
            const ClipRect text_clip_rect = clip_of(text_clip);
            /* The line box the caret and the selection highlight span. */
            const TextMetrics* line = world.get<TextMetrics>(entity);
            const float line_ascent =
                line != nullptr && line->ascent > 0.0F ? line->ascent : text->font_size * 0.8F;
            const float line_descent = line != nullptr && line->descent < 0.0F
                                           ? line->descent
                                           : -text->font_size * 0.2F;
            if (focused_ == entity && count > 0U)
            {
                std::uint32_t select_begin = 0U;
                std::uint32_t select_end = 0U;
                if (selectionRange(world, entity, select_begin, select_end) &&
                    select_begin < select_end)
                {
                    /* One column per selected glyph's cell, spanning the line
                       box. The theme grows a token for this when Text does. */
                    constexpr Vec4 kSelectionColor{0.20F, 0.40F, 0.70F, 0.40F};
                    for (std::size_t index = 0; index < count; ++index)
                    {
                        const detail::GlyphQuad& glyph = records[index];
                        if (glyph.byte_offset < select_begin ||
                            glyph.byte_offset >= select_end)
                        {
                            continue;
                        }
                        const float start = origin_x + glyph.pen_x;
                        const float end = index + 1U < count
                                              ? origin_x + records[index + 1U].pen_x
                                              : (line != nullptr && line->advance > glyph.pen_x
                                                     ? origin_x + line->advance
                                                     : start + glyph.width);
                        if (end <= start)
                        {
                            continue;
                        }
                        quads_.push_back(Quad{start, origin_y + glyph.pen_y - line_ascent,
                                              end - start, line_ascent - line_descent,
                                              kSelectionColor});
                        quads_.back().clip = text_clip_rect;
                    }
                }
            }
            if (label_visible && world.alive(text->content) && count > 0U)
            {
                TextRun run;
                run.font_size = text->font_size;
                run.color = text->text_color;
                run.clip = text_clip_rect;
                run.glyphs.reserve(count);
                for (std::size_t index = 0; index < count; ++index)
                {
                    const detail::GlyphQuad& glyph = records[index];
                    run.glyphs.push_back(TextGlyph{origin_x + glyph.pen_x, origin_y + glyph.pen_y,
                                                   glyph.glyph_id, glyph.font_index});
                }
                texts_.push_back(std::move(run));
            }
            for (std::size_t index = 0; index < count; ++index)
            {
                const detail::GlyphQuad& glyph = records[index];
                if (!visible(text_clip, origin_x + glyph.x, origin_y + glyph.y, glyph.width,
                             glyph.height))
                {
                    continue;
                }
                quads_.push_back(Quad{origin_x + glyph.x, origin_y + glyph.y, glyph.width,
                                      glyph.height, text->text_color, kTextureFont,
                                      Vec2{glyph.u0, glyph.v0}, Vec2{glyph.u1, glyph.v1}});
                quads_.back().clip = text_clip_rect;
            }
            const CaretBlink* blink = world.get<CaretBlink>(blink_source_);
            const bool caret_visible = blink == nullptr || blink->visible;
            if (focused_ == entity && caret_visible && isTextField(world, entity))
            {
                /* The caret sits where the shaped run ends; an empty field
                   starts it at the alignment edge. The ink extents cannot
                   answer this — atlas padding and trailing space advances are
                   not visible pixels. */
                float caret_x = rect->x;
                float caret_y = rect->y;
                /* The caret spans the line box, so its height does not follow
                   the letters next to it and an empty field still has one. */
                const float caret_height = line_ascent - line_descent;
                if (count > 0U)
                {
                    const std::size_t line = caret_index < count ? caret_index : count - 1U;
                    caret_y = origin_y + records[line].pen_y - line_ascent;
                    caret_x = rect->x + text->padding_left - scroll_x + caret_local;
                }
                else
                {
                    /* The empty field's caret follows the alignment inside the
                       padded span, where its first character would start. */
                    const float inner = std::fmax(1.0F, rect->width - text->padding_left);
                    if (text->align == TextAlign::Center)
                    {
                        caret_x = rect->x + text->padding_left + (inner - 1.0F) * 0.5F;
                    }
                    else if (text->align == TextAlign::End)
                    {
                        caret_x = rect->x + text->padding_left + inner - 1.0F;
                    }
                    else
                    {
                        caret_x = rect->x + text->padding_left;
                    }
                    if (text->v_align == VAlign::Middle)
                    {
                        caret_y = rect->y + (rect->height - caret_height) * 0.5F;
                    }
                    else if (text->v_align == VAlign::Bottom)
                    {
                        caret_y = rect->y + rect->height - caret_height;
                    }
                }
                if (visible(clip, caret_x, caret_y, 1.0F, caret_height))
                {
                    quads_.push_back(
                        Quad{caret_x, caret_y, 1.0F, caret_height, text->text_color});
                }
            }
        }
    }

    /* Everything the entity drew is confined to the clip it inherits; entries
       that already carry a tighter one keep it. */
    const ClipRect inherited = clip_of(clip);
    for (std::size_t index = quad_begin; index < quads_.size(); ++index)
    {
        if (quads_[index].clip.width <= 0.0F)
        {
            quads_[index].clip = inherited;
        }
    }

    const auto children = hierarchy::children(world, entity);
    const std::size_t start = order_.size();
    for (const ecs::Entity child : children)
    {
        if (!detail::childShown(world, entity, child))
        {
            continue;
        }
        const RectTransform* transform = world.get<RectTransform>(child);
        order_.push_back(OrderEntry{child, transform != nullptr ? transform->z : 0});
    }
    const std::size_t end = order_.size();
    std::stable_sort(order_.begin() + static_cast<std::ptrdiff_t>(start),
                     order_.begin() + static_cast<std::ptrdiff_t>(end),
                     [](const OrderEntry& left, const OrderEntry& right)
                     { return left.z < right.z; });

    /* The range stays valid across recursion: a child appends its own entries
       after `end`, never inside this one. */
    for (std::size_t index = start; index < end; ++index)
    {
        const ecs::Entity child = order_[index].entity;
        if (!paint(world, child, depth + 1U, clip))
        {
            return false;
        }
    }
    return true;
}

void DisplayList::clear()
{
    quads_.clear();
    texts_.clear();
    order_.clear();
    focused_ = ecs::Entity{};
}

void DisplayList::push(const Quad& quad)
{
    quads_.push_back(quad);
}

bool DisplayList::empty() const noexcept
{
    return quads_.empty();
}

std::size_t DisplayList::size() const noexcept
{
    return quads_.size();
}

Span<const Quad> DisplayList::quads() const noexcept
{
    return Span<const Quad>(quads_.data(), quads_.size());
}

Span<const TextRun> DisplayList::texts() const noexcept
{
    return Span<const TextRun>(texts_.data(), texts_.size());
}

bool buildDisplayList(ecs::World& world, ecs::Entity root, DisplayList& out)
{
    out.clear();
    out.blink_source_ = root;
    const PointerState* pointer = world.get<PointerState>(root);
    if (pointer != nullptr)
    {
        out.focused_ = pointer->focused;
    }
    return out.paint(world, root, 0U, DisplayList::Clip{});
}

} // namespace resonate::ui
