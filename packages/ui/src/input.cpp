#include <resonate/ui/input.h>

#include <algorithm>
#include <vector>

#include <resonate/ui/event.h>
#include <resonate/ui/hit_test.h>
#include <resonate/ui/text_runtime.h>

#include <resonate/hierarchy/tree.h>

#include "state_style_internal.h"
#include "resonate.ui/components.gen.h"
#include "widget_shown.h"

namespace resonate::ui
{
namespace
{

/* Finger travel past this slop turns a press into a drag, which cancels the
   click; under it the gesture stays a tap. In logical pixels like the pointer. */
constexpr float kDragSlop = 8.0F;

void setState(ecs::World& world, ecs::Entity entity, WidgetState state)
{
    Input* input = world.get<Input>(entity);
    if (input == nullptr || input->state == state)
    {
        return;
    }
    input->state = state;
    world.markChanged<Input>(entity);
    detail::applyStateStyles(world, entity, static_cast<std::uint32_t>(state));
}

/* The nearest Input-bearing ancestor of the hit widget. A non-focusable Input
   answers no one, which also stops the walk: a disabled child must not hand the
   pointer to the widget above it. */
ecs::Entity interactiveAt(ecs::World& world, ecs::Entity canvas, float x, float y)
{
    ecs::Entity entity = hitTest(world, canvas, x, y);
    for (std::uint32_t depth = 0U; entity.valid() && depth <= hierarchy::kMaxDepth; ++depth)
    {
        const Input* input = world.get<Input>(entity);
        if (input != nullptr)
        {
            return input->focusable ? entity : ecs::Entity{};
        }
        entity = hierarchy::parentOf(world, entity);
    }
    return ecs::Entity{};
}

/* The nearest widget on the hit chain that carries a Splitter. */
ecs::Entity splitterAt(ecs::World& world, ecs::Entity entity)
{
    for (std::uint32_t depth = 0U; entity.valid() && depth <= hierarchy::kMaxDepth; ++depth)
    {
        if (world.get<Splitter>(entity) != nullptr)
        {
            return entity;
        }
        entity = hierarchy::parentOf(world, entity);
    }
    return {};
}

/* The divider follows the pointer: the previous sibling grows by the motion,
   the next one moves and shrinks, and the divider travels with the edge. */
void applySplitter(ecs::World& world, ecs::Entity entity, float dx, float dy)
{
    const Splitter* splitter = world.get<Splitter>(entity);
    if (splitter == nullptr)
    {
        return;
    }
    const bool horizontal = splitter->axis == 0U;
    const float move = horizontal ? dx : dy;
    if (move == 0.0F)
    {
        return;
    }
    const ecs::Entity parent = hierarchy::parentOf(world, entity);
    if (!parent.valid())
    {
        return;
    }
    ecs::Entity previous{};
    ecs::Entity next{};
    bool passed = false;
    for (const ecs::Entity sibling : hierarchy::children(world, parent))
    {
        if (sibling == entity)
        {
            passed = true;
            continue;
        }
        if (!passed)
        {
            previous = sibling;
            continue;
        }
        next = sibling;
        break;
    }
    if (RectTransform* before = previous.valid() ? world.get<RectTransform>(previous) : nullptr)
    {
        if (horizontal)
        {
            before->width += move;
        }
        else
        {
            before->height += move;
        }
        world.markChanged<RectTransform>(previous);
    }
    if (RectTransform* after = next.valid() ? world.get<RectTransform>(next) : nullptr)
    {
        if (horizontal)
        {
            after->x += move;
            after->width -= move;
        }
        else
        {
            after->y += move;
            after->height -= move;
        }
        world.markChanged<RectTransform>(next);
    }
    if (RectTransform* own = world.get<RectTransform>(entity))
    {
        if (horizontal)
        {
            own->x += move;
        }
        else
        {
            own->y += move;
        }
        world.markChanged<RectTransform>(entity);
    }
}

/* The nearest viewport on the hit chain whose scrollbar thumb the point is
   inside; dragging one moves the scroll rather than the content. */
ecs::Entity scrollThumbAt(ecs::World& world, ecs::Entity entity, float x, float y)
{
    for (std::uint32_t depth = 0U; entity.valid() && depth <= hierarchy::kMaxDepth; ++depth)
    {
        const ScrollView* view = world.get<ScrollView>(entity);
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (view != nullptr && rect != nullptr)
        {
            const detail::ScrollThumb thumb = detail::scrollThumbOf(*rect, *view);
            if (thumb.visible && x >= thumb.x && x <= thumb.x + thumb.width && y >= thumb.y &&
                y <= thumb.y + thumb.height)
            {
                return entity;
            }
        }
        entity = hierarchy::parentOf(world, entity);
    }
    return {};
}

/* The nearest widget on the hit chain that carries a Scrubber. */
ecs::Entity scrubberAt(ecs::World& world, ecs::Entity entity)
{
    for (std::uint32_t depth = 0U; entity.valid() && depth <= hierarchy::kMaxDepth; ++depth)
    {
        if (world.get<Scrubber>(entity) != nullptr)
        {
            return entity;
        }
        entity = hierarchy::parentOf(world, entity);
    }
    return {};
}

/* Horizontal motion changes the value; the application owns the range. */
void applyScrubber(ecs::World& world, ecs::Entity entity, float dx)
{
    Scrubber* scrubber = world.get<Scrubber>(entity);
    if (scrubber == nullptr || dx == 0.0F)
    {
        return;
    }
    scrubber->value += dx * scrubber->step;
    world.markChanged<Scrubber>(entity);
}

/* The nearest widget on the hit chain that carries a Slider. */
ecs::Entity sliderAt(ecs::World& world, ecs::Entity entity)
{
    for (std::uint32_t depth = 0U; entity.valid() && depth <= hierarchy::kMaxDepth; ++depth)
    {
        if (world.get<Slider>(entity) != nullptr)
        {
            return entity;
        }
        entity = hierarchy::parentOf(world, entity);
    }
    return {};
}

/* A press or drag at `x` sets the slider's value from its position. */
void applySlider(ecs::World& world, ecs::Entity entity, float x)
{
    Slider* slider = world.get<Slider>(entity);
    const ComputedRect* rect = world.get<ComputedRect>(entity);
    if (slider == nullptr || rect == nullptr || !(rect->width > 0.0F) ||
        !(slider->max > slider->min))
    {
        return;
    }
    const float fraction = std::clamp((x - rect->x) / rect->width, 0.0F, 1.0F);
    const float value = slider->min + fraction * (slider->max - slider->min);
    if (value != slider->value)
    {
        slider->value = value;
        world.markChanged<Slider>(entity);
    }
}

void collectFocusable(ecs::World& world, ecs::Entity entity, std::vector<ecs::Entity>& out,
                      std::uint32_t depth)
{
    if (depth > hierarchy::kMaxDepth)
    {
        return;
    }
    const Input* input = world.get<Input>(entity);
    if (input != nullptr && input->focusable && input->state != WidgetState::Disabled)
    {
        out.push_back(entity);
    }
    for (const ecs::Entity child : hierarchy::children(world, entity))
    {
        /* Hidden pages and closed popups are not painted or hit-tested, so
           focus must not land in them either. */
        if (!detail::childShown(world, entity, child))
        {
            continue;
        }
        collectFocusable(world, child, out, depth + 1U);
    }
}

} // namespace

bool updateInput(ecs::World& world, ecs::Entity canvas, float x, float y, bool down,
                 double seconds)
{
    /* Attaching DragState moves the canvas to a new archetype, so PointerState
       is fetched only after the structural change; the pointers must not
       straddle it. */
    DragState* drag = world.get<DragState>(canvas);
    if (drag == nullptr)
    {
        drag = world.add(canvas, DragState{});
        if (drag == nullptr)
        {
            world.report(ecs::World::Report::Error,
                         "ui input: the canvas %u:%u rejected a DragState", canvas.index,
                         canvas.generation);
            return false;
        }
    }
    PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr)
    {
        world.report(ecs::World::Report::Error,
                     "ui input: entity %u:%u is not a canvas with PointerState", canvas.index,
                     canvas.generation);
        return false;
    }
    const float last_x = drag->x;
    const float last_y = drag->y;
    drag->x = x;
    drag->y = y;

    /* A release was reported once; clear it before this frame's sample so a
       reader sees it for exactly one update. */
    if (pointer->activated.valid())
    {
        if (world.alive(pointer->activated))
        {
            Input* input = world.get<Input>(pointer->activated);
            if (input != nullptr && input->activated)
            {
                input->activated = false;
                world.markChanged<Input>(pointer->activated);
            }
        }
        pointer->activated = ecs::Entity{};
        world.markChanged<PointerState>(canvas);
    }
    if (pointer->press.valid() && !world.alive(pointer->press))
    {
        pointer->press = ecs::Entity{};
        world.markChanged<PointerState>(canvas);
    }

    const ecs::Entity hover = interactiveAt(world, canvas, x, y);
    const ecs::Entity previous = pointer->hover;
    if (previous != hover)
    {
        pointer->hover = hover;
        world.markChanged<PointerState>(canvas);
        if (previous.valid() && world.alive(previous))
        {
            setState(world, previous, previous == pointer->focused ? WidgetState::Focused
                                                                   : WidgetState::Normal);
        }
    }

    /* A captured gesture keeps every move and the release; the pointer's own
       position no longer picks the target. */
    const bool captured = pointer->capture.valid() && world.alive(pointer->capture);
    const ecs::Entity routing = captured ? pointer->capture : ecs::Entity{};
    Event move{};
    move.kind = EventKind::PointerMove;
    move.x = x;
    move.y = y;
    dispatchEvent(world, canvas, move, routing);

    bool pressed_this_sample = false;
    if (down && !pointer->down)
    {
        pressed_this_sample = true;
        Event pressed = move;
        pressed.kind = EventKind::PointerDown;
        dispatchEvent(world, canvas, pressed);
        if (pointer->focused != hover && pointer->focused.valid() &&
            world.alive(pointer->focused))
        {
            setState(world, pointer->focused, WidgetState::Normal);
        }
        pointer->focused = hover;
        pointer->down = true;
        pointer->press = hover;
        world.markChanged<PointerState>(canvas);
        if (hover.valid())
        {
            /* A press inside a text field drops the caret where it landed and
               arms the selection: the anchor starts there, a drag moves the
               edge, a plain release leaves the range empty. A second press on
               the same field inside the double-click window selects the word
               instead. */
            const ComputedRect* rect = world.get<ComputedRect>(hover);
            if (rect != nullptr && isTextField(world, hover))
            {
                constexpr double kDoubleClickWindow = 0.4;
                const bool double_click =
                    seconds > drag->last_click_time &&
                    seconds - drag->last_click_time <= kDoubleClickWindow &&
                    drag->last_click_field == hover;
                if (double_click)
                {
                    (void)selectWordAt(world, hover, x - rect->x);
                    /* No triple-click chaining: the next press starts fresh. */
                    drag->last_click_time = -1.0;
                    drag->last_click_field = ecs::Entity{};
                }
                else
                {
                    (void)beginTextSelect(world, hover, x - rect->x);
                    drag->last_click_time = seconds;
                    drag->last_click_field = hover;
                }
                drag->text_field = hover;
            }
        }
        /* A press on a slider jumps its value there and owns the gesture; the
           release then neither activates nor clicks it. */
        /* Gesture widgets need not be Input widgets themselves (a divider is a
           plain box), so the lookup starts at the raw hit. A handler that
           captured the pointer owns the gesture instead. */
        const bool captured_now = pointer->capture.valid();
        const ecs::Entity hit = hitTest(world, canvas, x, y);
        drag->slider = captured_now ? ecs::Entity{} : sliderAt(world, hit);
        if (drag->slider.valid())
        {
            applySlider(world, drag->slider, x);
            pointer->press = ecs::Entity{};
            world.markChanged<PointerState>(canvas);
        }
        drag->splitter =
            captured_now || drag->slider.valid() ? ecs::Entity{} : splitterAt(world, hit);
        if (drag->splitter.valid())
        {
            pointer->press = ecs::Entity{};
            world.markChanged<PointerState>(canvas);
        }
        if (!captured_now && !drag->slider.valid() && !drag->splitter.valid())
        {
            drag->scrubber = scrubberAt(world, hit);
            if (drag->scrubber.valid())
            {
                pointer->press = ecs::Entity{};
                world.markChanged<PointerState>(canvas);
            }
        }
        if (!captured_now && !drag->slider.valid() && !drag->splitter.valid() &&
            !drag->scrubber.valid())
        {
            drag->scroll_thumb = scrollThumbAt(world, hit, x, y);
            if (drag->scroll_thumb.valid())
            {
                pointer->press = ecs::Entity{};
                world.markChanged<PointerState>(canvas);
            }
        }
        drag->pressed = true;
        drag->dragging = false;
        drag->press_x = x;
        drag->press_y = y;
        if (hover.valid())
        {
            setState(world, hover, WidgetState::Pressed);
        }
    }
    else if (!down && pointer->down)
    {
        pointer->down = false;
        world.markChanged<PointerState>(canvas);
        drag->pressed = false;
        drag->dragging = false;
        drag->slider = ecs::Entity{};
        drag->splitter = ecs::Entity{};
        drag->scrubber = ecs::Entity{};
        drag->scroll_thumb = ecs::Entity{};
        drag->text_field = ecs::Entity{};
        const ecs::Entity press = pointer->press;
        Event released = move;
        released.kind = EventKind::PointerUp;
        dispatchEvent(world, canvas, released, routing);
        if (pointer->capture.valid())
        {
            pointer->capture = ecs::Entity{};
            world.markChanged<PointerState>(canvas);
        }
        if (press.valid() && world.alive(press))
        {
            if (press == hover)
            {
                Input* input = world.get<Input>(press);
                if (input != nullptr)
                {
                    input->activated = true;
                    world.markChanged<Input>(press);
                    pointer->activated = press;
                    world.markChanged<PointerState>(canvas);
                }
                Event clicked = move;
                clicked.kind = EventKind::Click;
                dispatchEvent(world, canvas, clicked, routing);
            }
            setState(world, press, press == hover
                                      ? WidgetState::Hovered
                                      : (press == pointer->focused ? WidgetState::Focused
                                                                   : WidgetState::Normal));
        }
        pointer->press = ecs::Entity{};
        world.markChanged<PointerState>(canvas);
    }

    if (pointer->down && pointer->press.valid())
    {
        setState(world, pointer->press,
                 pointer->press == hover
                     ? WidgetState::Pressed
                     : (pointer->press == pointer->focused ? WidgetState::Focused
                                                           : WidgetState::Normal));
    }

    /* Once the finger travels past the slop the press becomes a drag: drop the
       pending press so the release neither activates nor clicks, and let the
       viewport chain under the finger follow it. */
    if (captured)
    {
        /* The handler that captured the pointer drives the gesture; the
           built-in scroll, slider, splitter and scrub stay out of it. */
    }
    else if (drag->pressed && pointer->down && drag->slider.valid())
    {
        if (!pressed_this_sample)
        {
            applySlider(world, drag->slider, x);
        }
    }
    else if (drag->pressed && pointer->down && drag->splitter.valid())
    {
        if (!pressed_this_sample)
        {
            applySplitter(world, drag->splitter, x - last_x, y - last_y);
        }
    }
    else if (drag->pressed && pointer->down && drag->scrubber.valid())
    {
        if (!pressed_this_sample)
        {
            applyScrubber(world, drag->scrubber, x - last_x);
        }
    }
    else if (drag->pressed && pointer->down && drag->scroll_thumb.valid())
    {
        /* The thumb follows the pointer one for one: its travel covers the
           whole range. */
        ScrollView* view = world.get<ScrollView>(drag->scroll_thumb);
        const ComputedRect* rect = world.get<ComputedRect>(drag->scroll_thumb);
        if (view != nullptr && rect != nullptr && !pressed_this_sample)
        {
            const detail::ScrollThumb thumb = detail::scrollThumbOf(*rect, *view);
            const float max_scroll = std::fmax(0.0F, view->content_height - rect->height);
            const float wanted =
                std::clamp(view->scroll + (y - last_y) * thumb.scroll_per_pixel, 0.0F, max_scroll);
            if (wanted != view->scroll)
            {
                view->scroll = wanted;
                world.markChanged<ScrollView>(drag->scroll_thumb);
            }
        }
    }
    else if (drag->pressed && pointer->down && drag->text_field.valid())
    {
        /* Selection drags have no slop: every sample the pointer moves moves
           the caret edge, which is how a short drag selects a few letters. */
        const ComputedRect* rect = world.get<ComputedRect>(drag->text_field);
        if (rect != nullptr && !pressed_this_sample)
        {
            (void)extendCaretTo(world, drag->text_field, x - rect->x);
        }
    }
    else if (drag->pressed && pointer->down)
    {
        if (!drag->dragging)
        {
            const float dx = x - drag->press_x;
            const float dy = y - drag->press_y;
            if (dx * dx + dy * dy > kDragSlop * kDragSlop)
            {
                drag->dragging = true;
                pointer->press = ecs::Entity{};
                world.markChanged<PointerState>(canvas);
            }
        }
        if (drag->dragging)
        {
            (void)dragScroll(world, canvas, x, y, y - last_y);
        }
    }

    if (hover.valid())
    {
        setState(world, hover,
                 pointer->down && pointer->press == hover ? WidgetState::Pressed
                                                          : WidgetState::Hovered);
    }
    return true;
}

CursorShape cursorShape(ecs::World& world, ecs::Entity canvas)
{
    /* A gesture owns the pointer, so it owns the cursor: the widget under the
       pointer is not what the drag is moving. */
    if (const DragState* drag = world.get<DragState>(canvas))
    {
        if (drag->slider.valid() || drag->scrubber.valid() || drag->splitter.valid())
        {
            return CursorShape::ResizeHorizontal;
        }
        if (drag->scroll_thumb.valid())
        {
            return CursorShape::ResizeVertical;
        }
        if (drag->text_field.valid())
        {
            return CursorShape::Text;
        }
    }
    const PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr || !pointer->hover.valid() || !world.alive(pointer->hover))
    {
        return CursorShape::Default;
    }
    const ecs::Entity hover = pointer->hover;
    if (isTextField(world, hover))
    {
        return CursorShape::Text;
    }
    if (const Input* input = world.get<Input>(hover))
    {
        if (!input->focusable || input->state == WidgetState::Disabled)
        {
            return CursorShape::NotAllowed;
        }
        return CursorShape::Pointer;
    }
    return CursorShape::Default;
}

bool focusNext(ecs::World& world, ecs::Entity canvas, bool backwards)
{
    PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr)
    {
        return false;
    }
    std::vector<ecs::Entity> focusable;
    collectFocusable(world, canvas, focusable, 0U);
    if (focusable.empty())
    {
        return false;
    }

    std::size_t current = focusable.size();
    for (std::size_t index = 0; index < focusable.size(); ++index)
    {
        if (focusable[index] == pointer->focused)
        {
            current = index;
            break;
        }
    }
    const std::size_t next =
        current == focusable.size()
            ? (backwards ? focusable.size() - 1U : 0U)
            : (backwards ? (current + focusable.size() - 1U) % focusable.size()
                         : (current + 1U) % focusable.size());
    const ecs::Entity target = focusable[next];
    if (pointer->focused != target)
    {
        if (pointer->focused.valid() && world.alive(pointer->focused))
        {
            setState(world, pointer->focused, pointer->hover == pointer->focused
                                                  ? WidgetState::Hovered
                                                  : WidgetState::Normal);
        }
        pointer->focused = target;
        world.markChanged<PointerState>(canvas);
    }
    setState(world, target,
             pointer->hover == target ? WidgetState::Hovered : WidgetState::Focused);
    return true;
}

bool scrollInput(ecs::World& world, ecs::Entity canvas, float x, float y, float detents)
{
    if (detents == 0.0F)
    {
        return false;
    }
    Event event{};
    event.kind = EventKind::Wheel;
    event.x = x;
    event.y = y;
    event.wheel = detents;
    return dispatchEvent(world, canvas, event);
}

} // namespace resonate::ui
