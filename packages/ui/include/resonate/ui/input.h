#ifndef RESONATE_UI_INPUT_H
#define RESONATE_UI_INPUT_H

#include <resonate/ecs/component.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::ui
{

/* Touch-drag bookkeeping the input system keeps beside PointerState on the
   canvas: the running pointer sample, where the press began and whether the
   motion has grown into a drag (past the slop, which also cancels the click).
   Runtime data, never authored, so it names itself like Listener. */
struct DragState
{
    bool pressed = false;
    bool dragging = false;
    float x = 0.0F;
    float y = 0.0F;
    float press_x = 0.0F;
    float press_y = 0.0F;
    /* The slider this gesture drives, invalid for any other drag. */
    ecs::Entity slider{};
    /* The divider this gesture drags, invalid for any other drag. */
    ecs::Entity splitter{};
    /* The property label this gesture scrubs, invalid for any other drag. */
    ecs::Entity scrubber{};
    /* The viewport whose scrollbar thumb this gesture drags. */
    ecs::Entity scroll_thumb{};
};

/* Whether the caret draws this instant: the UI frame flips it every half
   second and resets it visible whenever the field changes, so a typing user
   never sees the caret vanish. Runtime data, never authored. */
struct CaretBlink
{
    bool visible = true;
};

/* Samples the pointer once per frame against the canvas' retained tree.
   `x` and `y` are in the same space layout works in: the target size divided
   by Canvas::scale, which is window points when the canvas scale is the display
   scale. `down` is the left button.

   The nearest Input-bearing ancestor of the hit widget owns the pointer. A
   non-focusable Input consumes the pointer without reacting, so a disabled
   button also shields its parent. Hovered, pressed and the one-update
   `activated` flag live on the canvas' PointerState; Input::state drives the
   compiled state styles. Call it before layout and before anything reads
   activation.

   A press that moves past the drag slop becomes a drag: the viewport chain
   under the pointer follows the finger and the release neither activates nor
   clicks. */
[[nodiscard]] bool updateInput(ecs::World& world, ecs::Entity canvas, float x, float y, bool down);

/* Moves focus to the next (or previous) focusable Input in tree order and
   wraps. Non-focusable and disabled widgets are skipped; a canvas with no
   PointerState or no focusable widget answers false. */
[[nodiscard]] bool focusNext(ecs::World& world, ecs::Entity canvas, bool backwards);

/* Routes wheel detents (positive scrolls down) through the tree at the pointer
   position. Handlers see a Wheel event first; a ScrollView on the path then
   scrolls as its default action. Returns true when the wheel was consumed. */
[[nodiscard]] bool scrollInput(ecs::World& world, ecs::Entity canvas, float x, float y,
                               float detents);

} // namespace resonate::ui

namespace resonate::ecs
{

/* DragState is runtime data (the input system's own bookkeeping), never
   authored, so it has no schema declaration and names itself here. */
RESONATE_COMPONENT(resonate::ui::DragState, "resonate.ui.DragState");

RESONATE_COMPONENT(resonate::ui::CaretBlink, "resonate.ui.CaretBlink");

} // namespace resonate::ecs

#endif /* RESONATE_UI_INPUT_H */
