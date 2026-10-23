#include <resonate/ui/display_list.h>

#include <algorithm>
#include <cstdint>

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

bool DisplayList::paint(ecs::World& world, ecs::Entity entity, std::uint32_t depth)
{
    if (depth > hierarchy::kMaxDepth)
    {
        return report(world, entity, "sits below the UI tree's depth limit");
    }

    if (const Background* background = world.get<Background>(entity))
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (rect == nullptr)
        {
            return report(world, entity, "has Background but no ComputedRect");
        }
        quads_.push_back(Quad{rect->x, rect->y, rect->width, rect->height, background->color});
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
            quads_.push_back(Quad{rect->x, rect->y, rect->width, rect->height, image->tint,
                                  image->texture, image->uv_min, image->uv_max});
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
        if (glyphs != nullptr && world.alive(glyphs->records))
        {
            const ComputedRect* rect = world.get<ComputedRect>(entity);
            if (rect == nullptr)
            {
                return report(world, entity, "has Text but no ComputedRect");
            }
            const auto* records = static_cast<const detail::GlyphQuad*>(
                world.blobData(glyphs->records));
            const std::size_t count =
                world.blobSize(glyphs->records) / sizeof(detail::GlyphQuad);
            for (std::size_t index = 0; index < count; ++index)
            {
                const detail::GlyphQuad& glyph = records[index];
                quads_.push_back(Quad{rect->x + glyph.x, rect->y + glyph.y, glyph.width,
                                      glyph.height, text->text_color, kTextureFont,
                                      Vec2{glyph.u0, glyph.v0}, Vec2{glyph.u1, glyph.v1}});
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
        if (!paint(world, child, depth + 1U))
        {
            return false;
        }
    }
    return true;
}

void DisplayList::clear()
{
    quads_.clear();
    order_.clear();
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

bool buildDisplayList(ecs::World& world, ecs::Entity root, DisplayList& out)
{
    out.clear();
    return out.paint(world, root, 0U);
}

} // namespace resonate::ui
