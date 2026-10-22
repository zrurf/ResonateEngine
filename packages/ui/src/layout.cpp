#include <resonate/ui/layout.h>

#include <cstdint>

#include <resonate/hierarchy/tree.h>

#include "resonate.ui/components.gen.h"

namespace resonate::ui
{
namespace
{

struct Box
{
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

bool reportMissing(ecs::World& world, ecs::Entity entity, const char* component)
{
    world.report(ecs::World::Report::Error, "ui layout: entity %u:%u has no %s", entity.index,
                 entity.generation, component);
    return false;
}

Box contentBox(const Box& box, const LayoutGroup* group)
{
    if (group == nullptr)
    {
        return box;
    }
    Box content = box;
    content.x += group->padding_left;
    content.y += group->padding_top;
    content.width -= group->padding_left + group->padding_right;
    content.height -= group->padding_top + group->padding_bottom;
    if (content.width < 0.0F)
    {
        content.width = 0.0F;
    }
    if (content.height < 0.0F)
    {
        content.height = 0.0F;
    }
    return content;
}

bool writeRect(ecs::World& world, ecs::Entity entity, const Box& box, float scale)
{
    ComputedRect* rect = world.get<ComputedRect>(entity);
    if (rect == nullptr)
    {
        return reportMissing(world, entity, "ComputedRect");
    }

    const float x = box.x * scale;
    const float y = box.y * scale;
    const float width = box.width * scale;
    const float height = box.height * scale;
    if (rect->x == x && rect->y == y && rect->width == width && rect->height == height)
    {
        return true;
    }

    rect->x = x;
    rect->y = y;
    rect->width = width;
    rect->height = height;
    /* Unchanged widgets stay clean, so a retained renderer reacts only to the
       rectangles layout actually moved. */
    world.markChanged(entity, ecs::ComponentTraits<ComputedRect>::index);
    return true;
}

/* The parent fraction the widget is tied to, plus its offsets and size. */
Box anchoredBox(const Box& parent, const RectTransform& rect)
{
    Box box;
    box.width = (rect.anchor_max.x - rect.anchor_min.x) * parent.width + rect.width;
    box.height = (rect.anchor_max.y - rect.anchor_min.y) * parent.height + rect.height;
    if (box.width < 0.0F)
    {
        box.width = 0.0F;
    }
    if (box.height < 0.0F)
    {
        box.height = 0.0F;
    }
    box.x = parent.x + rect.anchor_min.x * parent.width + rect.x - rect.pivot.x * box.width;
    box.y = parent.y + rect.anchor_min.y * parent.height + rect.y - rect.pivot.y * box.height;
    return box;
}

bool placeChildren(ecs::World& world, ecs::Entity parent, const Box& parent_box, float scale,
                   std::uint32_t depth)
{
    if (depth > hierarchy::kMaxDepth)
    {
        world.report(ecs::World::Report::Error,
                     "ui layout: the tree under entity %u:%u is deeper than %u levels",
                     parent.index, parent.generation, hierarchy::kMaxDepth);
        return false;
    }

    const LayoutGroup* group = world.get<LayoutGroup>(parent);
    const Box content = contentBox(parent_box, group);
    const auto children = hierarchy::children(world, parent);

    if (group == nullptr)
    {
        for (const ecs::Entity child : children)
        {
            const RectTransform* rect = world.get<RectTransform>(child);
            if (rect == nullptr)
            {
                return reportMissing(world, child, "RectTransform");
            }
            const Box box = anchoredBox(content, *rect);
            if (!writeRect(world, child, box, scale) ||
                !placeChildren(world, child, box, scale, depth + 1U))
            {
                return false;
            }
        }
        return true;
    }

    const bool row = group->direction == FlexDirection::Row;
    const float gap = group->gap < 0.0F ? 0.0F : group->gap;
    const float main_extent = row ? content.width : content.height;
    const float cross_extent = row ? content.height : content.width;

    /* The flow consumes neighbours and gap; absolute children are placed from
       their anchors and take no room in it. */
    std::uint32_t flow_count = 0;
    float used = 0.0F;
    for (const ecs::Entity child : children)
    {
        const RectTransform* rect = world.get<RectTransform>(child);
        if (rect == nullptr)
        {
            return reportMissing(world, child, "RectTransform");
        }
        if (rect->position == PositionMode::Absolute)
        {
            continue;
        }
        const float size = row ? rect->width : rect->height;
        used += size > 0.0F ? size : 0.0F;
        ++flow_count;
    }
    if (flow_count > 1U)
    {
        used += gap * static_cast<float>(flow_count - 1U);
    }

    float free_space = main_extent - used;
    if (free_space < 0.0F)
    {
        free_space = 0.0F;
    }
    float cursor = 0.0F;
    float step = gap;
    switch (group->justify)
    {
    case Justify::Center:
        cursor = free_space * 0.5F;
        break;
    case Justify::End:
        cursor = free_space;
        break;
    case Justify::SpaceBetween:
        if (flow_count > 1U)
        {
            step = gap + free_space / static_cast<float>(flow_count - 1U);
        }
        break;
    case Justify::Start:
        break;
    }

    for (const ecs::Entity child : children)
    {
        const RectTransform* rect = world.get<RectTransform>(child);
        if (rect == nullptr)
        {
            return reportMissing(world, child, "RectTransform");
        }
        if (rect->position == PositionMode::Absolute)
        {
            const Box box = anchoredBox(content, *rect);
            if (!writeRect(world, child, box, scale) ||
                !placeChildren(world, child, box, scale, depth + 1U))
            {
                return false;
            }
            continue;
        }

        float main_size = row ? rect->width : rect->height;
        float cross_size = row ? rect->height : rect->width;
        if (main_size < 0.0F)
        {
            main_size = 0.0F;
        }
        if (cross_size < 0.0F)
        {
            cross_size = 0.0F;
        }

        float cross_position = 0.0F;
        switch (group->align)
        {
        case Align::Center:
            cross_position = (cross_extent - cross_size) * 0.5F;
            break;
        case Align::End:
            cross_position = cross_extent - cross_size;
            break;
        case Align::Stretch:
            cross_size = cross_extent;
            break;
        case Align::Start:
            break;
        }

        Box box;
        if (row)
        {
            box = Box{content.x + cursor, content.y + cross_position, main_size, cross_size};
        }
        else
        {
            box = Box{content.x + cross_position, content.y + cursor, cross_size, main_size};
        }
        cursor += main_size + step;

        if (!writeRect(world, child, box, scale) ||
            !placeChildren(world, child, box, scale, depth + 1U))
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool registerComponents(ecs::World& world)
{
    return world.registerComponent<Canvas>() != ecs::kInvalidComponent &&
           world.registerComponent<RectTransform>() != ecs::kInvalidComponent &&
           world.registerComponent<LayoutGroup>() != ecs::kInvalidComponent &&
           world.registerComponent<ComputedRect>() != ecs::kInvalidComponent &&
           world.registerComponent<Background>() != ecs::kInvalidComponent &&
           world.registerComponent<Image>() != ecs::kInvalidComponent &&
           world.registerComponent<Text>() != ecs::kInvalidComponent &&
           world.registerComponent<Glyphs>() != ecs::kInvalidComponent &&
           world.registerComponent<TextAtlasPage>() != ecs::kInvalidComponent &&
           world.registerComponent<DataContext>() != ecs::kInvalidComponent &&
           world.registerComponent<Bindings>() != ecs::kInvalidComponent &&
           world.registerComponent<Input>() != ecs::kInvalidComponent &&
           world.registerComponent<PointerState>() != ecs::kInvalidComponent &&
           world.registerComponent<StateStyles>() != ecs::kInvalidComponent &&
           world.registerComponent<StyleRef>() != ecs::kInvalidComponent;
}

bool layout(ecs::World& world, ecs::Entity canvas, float viewport_width, float viewport_height)
{
    const Canvas* root = world.get<Canvas>(canvas);
    if (root == nullptr)
    {
        world.report(ecs::World::Report::Error,
                     "ui layout: entity %u:%u is not a canvas (no Canvas component)",
                     canvas.index, canvas.generation);
        return false;
    }
    if (!(viewport_width > 0.0F) || !(viewport_height > 0.0F))
    {
        world.report(ecs::World::Report::Error,
                     "ui layout: viewport %g x %g is not positive", static_cast<double>(viewport_width),
                     static_cast<double>(viewport_height));
        return false;
    }

    const float scale = root->scale > 0.0F ? root->scale : 1.0F;
    const Box box{0.0F, 0.0F, viewport_width / scale, viewport_height / scale};
    if (!writeRect(world, canvas, box, scale))
    {
        return false;
    }
    return placeChildren(world, canvas, box, scale, 1U);
}

} // namespace resonate::ui
