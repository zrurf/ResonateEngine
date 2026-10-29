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

    /* Zero means "no bound"; the clamp never pushes a size up to a zero min or
       down to a zero max. */
    auto clamp_extent = [](float value, float minimum, float maximum)
    {
        if (minimum > 0.0F && value < minimum)
        {
            value = minimum;
        }
        if (maximum > 0.0F && value > maximum)
        {
            value = maximum;
        }
        return value;
    };

    /* Absolute children are placed from their anchors and take no room in the
       flow. */
    struct FlowItem
    {
        ecs::Entity entity{};
        float main = 0.0F;
        float cross = 0.0F;
        /* Margins in flow order: along the main axis and across it. */
        float main_before = 0.0F;
        float main_after = 0.0F;
        float cross_before = 0.0F;
        float cross_after = 0.0F;
        float grow = 0.0F;
        float shrink = 0.0F;
        float min_main = 0.0F;
        float max_main = 0.0F;
        float min_cross = 0.0F;
        float max_cross = 0.0F;
        SelfAlign align_self = SelfAlign::Auto;
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
        const LayoutItem* config = world.get<LayoutItem>(child);
        FlowItem item;
        item.entity = child;
        item.main = row ? size.x : size.y;
        item.cross = row ? size.y : size.x;
        if (config != nullptr)
        {
            item.main = clamp_extent(item.main, row ? config->min_width : config->min_height,
                                     row ? config->max_width : config->max_height);
            item.cross = clamp_extent(item.cross, row ? config->min_height : config->min_width,
                                      row ? config->max_height : config->max_width);
            item.main_before = row ? config->margin_left : config->margin_top;
            item.main_after = row ? config->margin_right : config->margin_bottom;
            item.cross_before = row ? config->margin_top : config->margin_left;
            item.cross_after = row ? config->margin_bottom : config->margin_right;
            item.grow = config->grow;
            item.shrink = config->shrink;
            item.min_main = row ? config->min_width : config->min_height;
            item.max_main = row ? config->max_width : config->max_height;
            item.min_cross = row ? config->min_height : config->min_width;
            item.max_cross = row ? config->max_height : config->max_width;
            item.align_self = config->align_self;
        }
        items.push_back(item);
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
       line wherever the next item would pass the content extent. Outer sizes
       (margins included) are what a line has to fit. */
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
            const float outer = item.main + item.main_before + item.main_after;
            if (group->wrap && index > line_begin && cursor + outer > main_extent)
            {
                lines.push_back(Line{line_begin, index, 0.0F, 0.0F});
                line_begin = index;
                cursor = 0.0F;
            }
            cursor += outer + (index > line_begin ? gap : 0.0F);
        }
        lines.push_back(Line{line_begin, items.size(), 0.0F, 0.0F});
    }

    float cross_used = 0.0F;
    for (Line& line : lines)
    {
        const std::size_t count = line.end - line.begin;
        for (std::size_t index = line.begin; index < line.end; ++index)
        {
            const FlowItem& item = items[index];
            line.used += item.main + item.main_before + item.main_after;
            line.cross =
                std::fmax(line.cross, item.cross + item.cross_before + item.cross_after);
        }
        if (count > 1U)
        {
            line.used += gap * static_cast<float>(count - 1U);
        }

        /* Shrink is grow's mirror on an overflowing line: each item gives back
           its scaled share (shrink times its own base, so wide items yield
           more), frozen at a min and the rest of the overflow redistributes
           over the still-free bases. Max cannot bind here — shrink only goes
           down, and the base already respected it. */
        float overflow = line.used - main_extent;
        if (overflow > 0.01F)
        {
            std::vector<float> base(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                base[index] = items[line.begin + index].main;
            }
            std::vector<bool> frozen(count, false);
            for (bool again = true; again && overflow > 0.01F;)
            {
                again = false;
                float scale_total = 0.0F;
                for (std::size_t index = 0; index < count; ++index)
                {
                    const FlowItem& item = items[line.begin + index];
                    if (!frozen[index] && base[index] > 0.0F)
                    {
                        scale_total += item.shrink * base[index];
                    }
                }
                if (scale_total <= 0.0F)
                {
                    break;
                }
                bool froze_any = false;
                float freed = 0.0F;
                for (std::size_t index = 0; index < count; ++index)
                {
                    FlowItem& item = items[line.begin + index];
                    if (frozen[index] || base[index] <= 0.0F || item.shrink <= 0.0F)
                    {
                        continue;
                    }
                    const float wanted =
                        base[index] - overflow * (item.shrink * base[index]) / scale_total;
                    const float shrunk = clamp_extent(wanted, item.min_main, 0.0F);
                    if (shrunk > wanted + 0.01F)
                    {
                        item.main = shrunk;
                        frozen[index] = true;
                        froze_any = true;
                        freed += base[index] - shrunk;
                    }
                }
                if (froze_any)
                {
                    overflow -= freed;
                    again = true;
                }
                else
                {
                    for (std::size_t index = 0; index < count; ++index)
                    {
                        FlowItem& item = items[line.begin + index];
                        if (!frozen[index] && base[index] > 0.0F && item.shrink > 0.0F)
                        {
                            item.main = clamp_extent(
                                base[index] - overflow * (item.shrink * base[index]) /
                                                  scale_total,
                                item.min_main, 0.0F);
                        }
                    }
                    overflow = 0.0F;
                }
            }
        }

        /* Shrink changed the sizes, so the line's extent — and with it the
           scroll content height and the space grow may hand out — comes from
           the shrunk ones. */
        line.used = count > 1U ? gap * static_cast<float>(count - 1U) : 0.0F;
        for (std::size_t index = line.begin; index < line.end; ++index)
        {
            const FlowItem& item = items[index];
            line.used += item.main + item.main_before + item.main_after;
        }

        /* Grow hands the free space out in proportion; an item that hits its
           min or max freezes at it and the rest of the free space redistributes
           over the original base sizes of the growers still free. What remains
           is what justify spreads. A line that overflows (or has no growers)
           keeps its sizes, so justify behaves exactly as it did. */
        float free_space = std::fmax(0.0F, main_extent - line.used);
        if (free_space > 0.01F)
        {
            std::vector<float> base(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                base[index] = items[line.begin + index].main;
            }
            std::vector<bool> frozen(count, false);
            for (bool again = true; again && free_space > 0.01F;)
            {
                again = false;
                float active_grow = 0.0F;
                for (std::size_t index = 0; index < count; ++index)
                {
                    if (!frozen[index] && items[line.begin + index].grow > 0.0F)
                    {
                        active_grow += items[line.begin + index].grow;
                    }
                }
                if (active_grow <= 0.0F)
                {
                    break;
                }
                const float share = free_space / active_grow;
                float frozen_used = 0.0F;
                bool froze_any = false;
                for (std::size_t index = 0; index < count; ++index)
                {
                    FlowItem& item = items[line.begin + index];
                    if (frozen[index] || item.grow <= 0.0F)
                    {
                        continue;
                    }
                    const float wanted = base[index] + share * item.grow;
                    const float grown = clamp_extent(wanted, item.min_main, item.max_main);
                    if (grown < wanted - 0.01F)
                    {
                        item.main = grown;
                        frozen[index] = true;
                        froze_any = true;
                        frozen_used += grown - base[index];
                    }
                }
                if (froze_any)
                {
                    free_space -= frozen_used;
                    again = true;
                }
                else
                {
                    for (std::size_t index = 0; index < count; ++index)
                    {
                        FlowItem& item = items[line.begin + index];
                        if (!frozen[index] && item.grow > 0.0F)
                        {
                            item.main = base[index] + share * item.grow;
                        }
                    }
                    free_space = 0.0F;
                }
            }
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
            if (count > 1U)
            {
                step = gap + free_space / static_cast<float>(count - 1U);
            }
            break;
        case Justify::Start:
            break;
        }

        /* One unwrapped line stretches across the whole content; each wrapped
           line uses its own tallest item. align_self overrides the container
           for one item, and margins set the item's cross inset. */
        const float line_extent = group->wrap ? line.cross : cross_extent;
        for (std::size_t index = line.begin; index < line.end; ++index)
        {
            const FlowItem& item = items[index];
            const Align align = item.align_self == SelfAlign::Auto
                                    ? group->align
                                    : static_cast<Align>(static_cast<std::uint8_t>(
                                          item.align_self) - 1U);
            float cross_size = item.cross;
            float cross_position = item.cross_before;
            switch (align)
            {
            case Align::Center:
                cross_position =
                    (line_extent - cross_size - item.cross_before - item.cross_after) * 0.5F +
                    item.cross_before;
                break;
            case Align::End:
                cross_position =
                    line_extent - cross_size - item.cross_after;
                break;
            case Align::Stretch:
                cross_size =
                    clamp_extent(line_extent - item.cross_before - item.cross_after,
                                 item.min_cross, item.max_cross);
                cross_position = item.cross_before;
                break;
            case Align::Start:
                break;
            }

            Box box;
            if (row)
            {
                cursor += item.main_before;
                box = Box{content.x + cursor, content.y + cross_used + cross_position, item.main,
                          cross_size};
                cursor += item.main + item.main_after;
            }
            else
            {
                cursor += item.main_before;
                box = Box{content.x + cross_used + cross_position, content.y + cursor, cross_size,
                          item.main};
                cursor += item.main + item.main_after;
            }
            cursor += step;
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
           world.registerComponent<LayoutItem>() != ecs::kInvalidComponent &&
           world.registerComponent<ComputedRect>() != ecs::kInvalidComponent &&
           world.registerComponent<Background>() != ecs::kInvalidComponent &&
           world.registerComponent<Image>() != ecs::kInvalidComponent &&
           world.registerComponent<Text>() != ecs::kInvalidComponent &&
           world.registerComponent<Glyphs>() != ecs::kInvalidComponent &&
           world.registerComponent<TextMetrics>() != ecs::kInvalidComponent &&
           world.registerComponent<TextCaret>() != ecs::kInvalidComponent &&
           world.registerComponent<TextSelection>() != ecs::kInvalidComponent &&
           world.registerComponent<Checkbox>() != ecs::kInvalidComponent &&
           world.registerComponent<Slider>() != ecs::kInvalidComponent &&
           world.registerComponent<Dropdown>() != ecs::kInvalidComponent &&
           world.registerComponent<DropdownList>() != ecs::kInvalidComponent &&
           world.registerComponent<Tabs>() != ecs::kInvalidComponent &&
           world.registerComponent<TabPage>() != ecs::kInvalidComponent &&
           world.registerComponent<Tab>() != ecs::kInvalidComponent &&
           world.registerComponent<Splitter>() != ecs::kInvalidComponent &&
           world.registerComponent<Shown>() != ecs::kInvalidComponent &&
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
           world.registerComponent<CaretBlink>() != ecs::kInvalidComponent &&
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
