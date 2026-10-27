#include <resonate/ui/display_list.h>

#include <algorithm>
#include <cstdint>
#include <utility>

#include <resonate/hierarchy/tree.h>

#include "glyph_run.h"
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

    Clip clip = outer;
    if (world.get<ScrollView>(entity) != nullptr)
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has ScrollView but no ComputedRect");
        }
        clip = intersect(outer, rect->x, rect->y, rect->width, rect->height);
    }

    if (const Background* background = world.get<Background>(entity))
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has Background but no ComputedRect");
        }
        if (visible(clip, rect->x, rect->y, rect->width, rect->height))
        {
            quads_.push_back(Quad{rect->x, rect->y, rect->width, rect->height, background->color});
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
            if (label_visible && world.alive(text->content) && count > 0U)
            {
                TextRun run;
                run.font_size = text->font_size;
                run.color = text->text_color;
                run.glyphs.reserve(count);
                for (std::size_t index = 0; index < count; ++index)
                {
                    const detail::GlyphQuad& glyph = records[index];
                    run.glyphs.push_back(TextGlyph{rect->x + glyph.pen_x, rect->y + glyph.pen_y,
                                                   glyph.glyph_id, glyph.font_index});
                }
                texts_.push_back(std::move(run));
            }
            for (std::size_t index = 0; index < count; ++index)
            {
                const detail::GlyphQuad& glyph = records[index];
                if (!visible(clip, rect->x + glyph.x, rect->y + glyph.y, glyph.width,
                             glyph.height))
                {
                    continue;
                }
                quads_.push_back(Quad{rect->x + glyph.x, rect->y + glyph.y, glyph.width,
                                      glyph.height, text->text_color, kTextureFont,
                                      Vec2{glyph.u0, glyph.v0}, Vec2{glyph.u1, glyph.v1}});
            }
            if (focused_ == entity)
            {
                /* The caret sits where the shaped run ends; an empty field
                   starts it at the alignment edge. The ink extents cannot
                   answer this — atlas padding and trailing space advances are
                   not visible pixels. */
                float caret_x = rect->x;
                float caret_y = rect->y;
                const float caret_height = text->font_size > 0.0F ? text->font_size
                                                                  : rect->height;
                if (count > 0U)
                {
                    const detail::GlyphQuad& last = records[count - 1U];
                    caret_y = rect->y + last.y;
                    const TextMetrics* metrics = world.get<TextMetrics>(entity);
                    float offset_x = metrics != nullptr ? metrics->advance
                                                        : last.x + last.width + 1.0F;
                    /* A caret in the middle of the text sits at the pen of the
                       glyph whose cluster follows it; a field without a caret
                       component keeps it at the end. */
                    const TextCaret* caret = world.get<TextCaret>(entity);
                    if (caret != nullptr)
                    {
                        for (std::size_t index = 0; index < count; ++index)
                        {
                            if (records[index].byte_offset >= caret->offset)
                            {
                                offset_x = records[index].pen_x;
                                break;
                            }
                        }
                    }
                    caret_x = rect->x + offset_x;
                }
                else if (text->align == TextAlign::Center)
                {
                    caret_x = rect->x + (rect->width - 1.0F) * 0.5F;
                }
                else if (text->align == TextAlign::End)
                {
                    caret_x = rect->x + rect->width - 1.0F;
                }
                if (visible(clip, caret_x, caret_y, 1.0F, caret_height))
                {
                    quads_.push_back(
                        Quad{caret_x, caret_y, 1.0F, caret_height, text->text_color});
                }
            }
        }
    }

    const auto children = hierarchy::children(world, entity);
    const std::size_t start = order_.size();
    for (const ecs::Entity child : children)
    {
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
    const PointerState* pointer = world.get<PointerState>(root);
    if (pointer != nullptr)
    {
        out.focused_ = pointer->focused;
    }
    return out.paint(world, root, 0U, DisplayList::Clip{});
}

} // namespace resonate::ui
