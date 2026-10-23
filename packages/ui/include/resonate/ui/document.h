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
   warning is reported. Reports and answers an invalid entity on a malformed
   component value or an unknown component. */
[[nodiscard]] ecs::Entity instantiate(
    ecs::World& world, dom::Document& document,
    const dom::ComponentRegistry* components = nullptr,
    std::vector<std::shared_ptr<const text::Font>> fonts = {},
    const StringTable* strings = nullptr);

/* Instantiates through a live TextRuntime, whose font chain owns the atlas and
   allows setText after the document is built. */
[[nodiscard]] ecs::Entity instantiate(ecs::World& world, dom::Document& document,
                                      const dom::ComponentRegistry* components,
                                      TextRuntime& text,
                                      const StringTable* strings = nullptr);

} // namespace resonate::ui

#endif /* RESONATE_UI_DOCUMENT_H */
