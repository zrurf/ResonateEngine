#ifndef RESONATE_UI_LAYOUT_H
#define RESONATE_UI_LAYOUT_H

#include <resonate/ecs/world.h>

/*
 * Retained UI layout.
 *
 * A widget is an entity with RectTransform and ComputedRect, linked to its
 * parent through the hierarchy package; the root carries Canvas. One pass
 * walks the tree in hierarchy order, places every child from its anchors or
 * from its parent's LayoutGroup, and writes the result into ComputedRect in
 * target pixels. Layout never changes structure, so it can run inside a
 * world-aware system.
 *
 * Authored numbers are logical pixels; Canvas::scale is applied when a
 * rectangle is written, which is what keeps the tree independent of the
 * target's resolution.
 */
namespace resonate::ui
{

/* Registers the schema-declared widget components in a world. Call after the
   hierarchy's components; registering again is harmless and a refusal (a name
   collision) answers false. */
bool registerComponents(ecs::World& world);

/* Lays out the tree rooted at `canvas` into a viewport of the given size. The
   canvas needs Canvas and ComputedRect; every other visited entity needs
   RectTransform and ComputedRect, and a container's children are placed by its
   LayoutGroup when it has one. Reports and answers false on a missing
   component, a non-canvas root, a non-positive viewport, or a tree deeper than
   the hierarchy's bound. */
bool layout(ecs::World& world, ecs::Entity canvas, float viewport_width, float viewport_height);

} // namespace resonate::ui

#endif /* RESONATE_UI_LAYOUT_H */
