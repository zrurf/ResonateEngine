#ifndef RESONATE_UI_DOCUMENT_H
#define RESONATE_UI_DOCUMENT_H

#include <memory>

#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>
#include <resonate/text/font.h>

namespace resonate::ui
{

/* Instantiates a document parsed with documentSchema() into widget entities.
   Every node becomes one entity, components become ECS components, child nodes
   become hierarchy children and ComputedRect is attached as the derived
   layout output. The root must carry Canvas. A component registry, when given,
   binds components from other modules (scene/2D) through their schema
   descriptors. When a font is given, Label text is shaped and rasterized into
   the canvas's shared TextAtlasPage. Reports and answers an invalid entity on a
   malformed component value or an unknown component. */
[[nodiscard]] ecs::Entity instantiate(
    ecs::World& world, dom::Document& document,
    const dom::ComponentRegistry* components = nullptr,
    std::shared_ptr<const text::Font> font = nullptr);

} // namespace resonate::ui

#endif /* RESONATE_UI_DOCUMENT_H */
