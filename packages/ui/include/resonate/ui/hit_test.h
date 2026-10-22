#ifndef RESONATE_UI_HIT_TEST_H
#define RESONATE_UI_HIT_TEST_H

#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

/*
 * Pointer hit testing over the retained widget tree.
 *
 * The search is the reverse of painting: a child is tested before its parent,
 * and siblings by descending RectTransform::z and reversed hierarchy order, so
 * the topmost widget at a point is the one the eye sees there. Only entities
 * with ComputedRect participate; a widget without one is transparent to the
 * search.
 */
namespace resonate::ui
{

/* The topmost widget containing the point in the subtree rooted at `root`, or
   an invalid handle when nothing does. Reports nothing: a point in empty space
   is an ordinary answer. */
[[nodiscard]] ecs::Entity hitTest(ecs::World& world, ecs::Entity root, float x, float y);

} // namespace resonate::ui

#endif /* RESONATE_UI_HIT_TEST_H */
