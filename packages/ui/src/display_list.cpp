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
        quads_.push_back(Quad{rect->x, rect->y, rect->width, rect->height, image->tint,
                              image->texture, image->uv_min, image->uv_max});
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
