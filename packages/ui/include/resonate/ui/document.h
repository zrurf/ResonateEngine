#ifndef RESONATE_UI_DOCUMENT_H
#define RESONATE_UI_DOCUMENT_H

#include <memory>
#include <vector>

#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>
#include <resonate/text/font.h>

namespace resonate::ui
{

class IconRegistry;
class ImageRegistry;
class StringTable;
class TextRuntime;

/* Instantiates a document parsed with documentSchema() into widget entities.
   Every node becomes one entity, components become ECS components, child nodes
   become hierarchy children and ComputedRect is attached as the derived
   layout output. The root must carry Canvas. A component registry, when given,
   binds components from other modules (scene/2D) through their schema
   descriptors. When fonts are given, Label text is shaped through the chain and
   rasterized into the canvas's shared TextAtlasPage; the first font supplies
   the line metrics and the rest cover missing glyphs. A text beginning with
   '#' is a string-table key; without a table the key text is shown and a
   warning is reported. A Picture `path` resolves through the image registry and
   an Icon `icon` through the icon registry; without one, or with an
   unregistered name, the authored texture and UV rect stay and a warning is
   reported. Reports and answers an invalid entity on a malformed component
   value or an unknown component. */
[[nodiscard]] ecs::Entity instantiate(ecs::World& world, dom::Document& document,
                                      const dom::ComponentRegistry* components = nullptr,
                                      std::vector<std::shared_ptr<const text::Font>> fonts = {},
                                      const StringTable* strings = nullptr,
                                      const ImageRegistry* images = nullptr,
                                      const IconRegistry* icons = nullptr);

/* Instantiates through a live TextRuntime, whose font chain owns the atlas and
   allows setText after the document is built. */
[[nodiscard]] ecs::Entity instantiate(ecs::World& world, dom::Document& document,
                                      const dom::ComponentRegistry* components,
                                      TextRuntime& text, const StringTable* strings = nullptr,
                                      const ImageRegistry* images = nullptr,
                                      const IconRegistry* icons = nullptr);

/* The widget authored under `key`, found through the WidgetKey every keyed node
   carries — the document itself is gone by the time an application looks. An
   unknown key answers an invalid entity. */
[[nodiscard]] ecs::Entity findByKey(ecs::World& world, ecs::Entity canvas, std::string_view key);

/* The child of `container` whose Selectable is selected, or an invalid entity —
   the one choice a list holds. */
[[nodiscard]] ecs::Entity selectedChild(ecs::World& world, ecs::Entity container);

/* The text a row shows: the entity's own Text, or the first descendant's when
   the row is a button wrapping a label. Empty for a widget without text. */
[[nodiscard]] std::string widgetText(ecs::World& world, ecs::Entity entity);

/* A scrubber label's current value; false when the widget is not one. The
   value moves by dragging the label; the application reads it and clamps it to
   its own range. */
[[nodiscard]] bool scrubValue(ecs::World& world, ecs::Entity entity, float& out_value);

/* Sets the scrubber's value from application state, keeping the drag coherent
   when the two are kept in sync. */
[[nodiscard]] bool setScrubValue(ecs::World& world, ecs::Entity entity, float value);

/* Whether the widget paints, hit-tests and takes focus: its explicit toggle
   when it carries one, else true. */
[[nodiscard]] bool isShown(ecs::World& world, ecs::Entity entity);

/* Hides or shows the widget without destroying it, so its state survives the
   toggle. Layout still places a hidden widget: an application that hides a
   pane collapses its geometry itself. */
void setShown(ecs::World& world, ecs::Entity entity, bool shown);

} // namespace resonate::ui

#endif /* RESONATE_UI_DOCUMENT_H */
