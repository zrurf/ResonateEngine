#include <resonate/ui/layout.h>

#include <cstdint>

#include <resonate/hierarchy/tree.h>
#include <resonate/ui/event.h>
#include <resonate/ui/input.h>
#include <resonate/ui/text_runtime.h>

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

/* A zero authored dimension takes the widget's measured text extent. */
Vec2 effectiveSize(ecs::World& world, ecs::Entity entity, const RectTransform& rect)
{
    Vec2 size{rect.width, rect.height};
    if (const TextMetrics* metrics = world.get<TextMetrics>(entity))
    {
        if (size.x <= 0.0F)
        {
            size.x = metrics->width;
        }
        if (size.y <= 0.0F)
        {
            size.y = metrics->height;
        }
    }
    return size;
}

/* The parent fraction the widget is tied to, plus its offsets and size. */
Box anchoredBox(ecs::World& world, ecs::Entity entity, const Box& parent,
                const RectTransform& rect)
{
    const Vec2 size = effectiveSize(world, entity, rect);
    Box box;
    box.width = (rect.anchor_max.x - rect.anchor_min.x) * parent.width + size.x;
    box.height = (rect.anchor_max.y - rect.anchor_min.y) * parent.height + size.y;
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

    /* A ScrollView moves its subtree up by the scroll offset; the content box
       keeps its size so children keep their authored coordinates. */
    const ScrollView* view = world.get<ScrollView>(parent);
    const float scroll = view != nullptr ? view->scroll : 0.0F;

    if (group == nullptr)
    {
        for (const ecs::Entity child : children)
        {
            const RectTransform* rect = world.get<RectTransform>(child);
            if (rect == nullptr)
            {
                return reportMissing(world, child, "RectTransform");
            }
            Box box = anchoredBox(world, child, content, *rect);
            box.y -= scroll;
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

    /* Absolute children are placed from their anchors and take no room in the
       flow. */
    struct FlowItem
    {
        ecs::Entity entity{};
        float main = 0.0F;
        float cross = 0.0F;
    };
    std::vector<FlowItem> items;
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
        const Vec2 size = effectiveSize(world, child, *rect);
        const float main_size = row ? size.x : size.y;
        const float cross_size = row ? size.y : size.x;
        items.push_back(FlowItem{child, main_size > 0.0F ? main_size : 0.0F,
                                 cross_size > 0.0F ? cross_size : 0.0F});
    }

    /* Absolute children sit where their anchors put them and take no part in
       the lines. */
    for (const ecs::Entity child : children)
    {
        const RectTransform* rect = world.get<RectTransform>(child);
        if (rect == nullptr || rect->position != PositionMode::Absolute)
        {
            continue;
        }
        Box box = anchoredBox(world, child, content, *rect);
        box.y -= scroll;
        if (!writeRect(world, child, box, scale) ||
            !placeChildren(world, child, box, scale, depth + 1U))
        {
            return false;
        }
    }

    /* Lines: a non-wrapping group flows on one; a wrapping one starts a new
       line wherever the next item would pass the content extent. */
    struct Line
    {
        std::size_t begin = 0U;
        std::size_t end = 0U;
        float used = 0.0F;
        float cross = 0.0F;
    };
    std::vector<Line> lines;
    {
        std::size_t line_begin = 0U;
        float cursor = 0.0F;
        for (std::size_t index = 0; index < items.size(); ++index)
        {
            const FlowItem& item = items[index];
            if (group->wrap && index > line_begin && cursor + item.main > main_extent)
            {
                lines.push_back(Line{line_begin, index, 0.0F, 0.0F});
                line_begin = index;
                cursor = 0.0F;
            }
            cursor += item.main + (index > line_begin ? gap : 0.0F);
        }
        lines.push_back(Line{line_begin, items.size(), 0.0F, 0.0F});
    }

    float cross_used = 0.0F;
    for (Line& line : lines)
    {
        const std::size_t count = line.end - line.begin;
        for (std::size_t index = line.begin; index < line.end; ++index)
        {
            line.used += items[index].main;
            line.cross = std::fmax(line.cross, items[index].cross);
        }
        if (count > 1U)
        {
            line.used += gap * static_cast<float>(count - 1U);
        }

        const float free_space = std::fmax(0.0F, main_extent - line.used);
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
            if (count > 1U)
            {
                step = gap + free_space / static_cast<float>(count - 1U);
            }
            break;
        case Justify::Start:
            break;
        }

        /* One unwrapped line stretches across the whole content; each wrapped
           line uses its own tallest item. */
        const float line_extent = group->wrap ? line.cross : cross_extent;
        for (std::size_t index = line.begin; index < line.end; ++index)
        {
            const FlowItem& item = items[index];
            float cross_size = item.cross;
            float cross_position = 0.0F;
            switch (group->align)
            {
            case Align::Center:
                cross_position = (line_extent - cross_size) * 0.5F;
                break;
            case Align::End:
                cross_position = line_extent - cross_size;
                break;
            case Align::Stretch:
                cross_size = line_extent;
                break;
            case Align::Start:
                break;
            }

            Box box;
            if (row)
            {
                box = Box{content.x + cursor, content.y + cross_used + cross_position, item.main,
                          cross_size};
            }
            else
            {
                box = Box{content.x + cross_used + cross_position, content.y + cursor, cross_size,
                          item.main};
            }
            cursor += item.main + step;
            box.y -= scroll;

            if (!writeRect(world, item.entity, box, scale) ||
                !placeChildren(world, item.entity, box, scale, depth + 1U))
            {
                return false;
            }
        }
        cross_used += line_extent + gap;
    }
    if (!lines.empty())
    {
        cross_used -= gap;
    }

    if (view != nullptr && !row)
    {
        /* In target pixels, like ComputedRect: the wheel handler clamps the
           scroll range against the viewport it sees there. A wrapping group
           flows sideways when it wraps, so its height is the lines' extent. */
        float flowed = 0.0F;
        for (const Line& line : lines)
        {
            flowed += line.used;
        }
        if (group->wrap)
        {
            flowed = cross_used;
        }
        ScrollView* measured = world.get<ScrollView>(parent);
        const float content_height = flowed * scale;
        if (measured->content_height != content_height)
        {
            measured->content_height = content_height;
            world.markChanged<ScrollView>(parent);
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
           world.registerComponent<TextMetrics>() != ecs::kInvalidComponent &&
           world.registerComponent<TextCaret>() != ecs::kInvalidComponent &&
           world.registerComponent<Checkbox>() != ecs::kInvalidComponent &&
           world.registerComponent<Slider>() != ecs::kInvalidComponent &&
           world.registerComponent<Dropdown>() != ecs::kInvalidComponent &&
           world.registerComponent<DropdownList>() != ecs::kInvalidComponent &&
           world.registerComponent<Tabs>() != ecs::kInvalidComponent &&
           world.registerComponent<TabPage>() != ecs::kInvalidComponent &&
           world.registerComponent<Tab>() != ecs::kInvalidComponent &&
           world.registerComponent<Splitter>() != ecs::kInvalidComponent &&
           world.registerComponent<Selectable>() != ecs::kInvalidComponent &&
           world.registerComponent<Scrubber>() != ecs::kInvalidComponent &&
           world.registerComponent<WidgetKey>() != ecs::kInvalidComponent &&
           world.registerComponent<TextRuntimeRef>() != ecs::kInvalidComponent &&
           world.registerComponent<Transition>() != ecs::kInvalidComponent &&
           world.registerComponent<ColorTween>() != ecs::kInvalidComponent &&
           world.registerComponent<TextAtlasPage>() != ecs::kInvalidComponent &&
           world.registerComponent<DataContext>() != ecs::kInvalidComponent &&
           world.registerComponent<Bindings>() != ecs::kInvalidComponent &&
           world.registerComponent<ScrollView>() != ecs::kInvalidComponent &&
           world.registerComponent<Listener>() != ecs::kInvalidComponent &&
           world.registerComponent<Input>() != ecs::kInvalidComponent &&
           world.registerComponent<PointerState>() != ecs::kInvalidComponent &&
           world.registerComponent<DragState>() != ecs::kInvalidComponent &&
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
