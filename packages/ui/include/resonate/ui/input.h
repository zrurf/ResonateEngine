#ifndef RESONATE_UI_INPUT_H
#define RESONATE_UI_INPUT_H

#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::ui
{

/* Samples the pointer once per frame against the canvas' retained tree.
   `x` and `y` are in the same space layout works in: the target size divided
   by Canvas::scale, which is window points when the canvas scale is the display
   scale. `down` is the left button.

   The nearest Input-bearing ancestor of the hit widget owns the pointer. A
   non-focusable Input consumes the pointer without reacting, so a disabled
   button also shields its parent. Hovered, pressed and the one-update
   `activated` flag live on the canvas' PointerState; Input::state drives the
   compiled state styles. Call it before layout and before anything reads
   activation. */
[[nodiscard]] bool updateInput(ecs::World& world, ecs::Entity canvas, float x, float y, bool down);

} // namespace resonate::ui

#endif /* RESONATE_UI_INPUT_H */
