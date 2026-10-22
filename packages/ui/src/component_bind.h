#ifndef RESONATE_UI_COMPONENT_BIND_H
#define RESONATE_UI_COMPONENT_BIND_H

#include <string>

#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::ui::detail
{

/* Writes one field's text into a packed component payload through its schema
   layout. Shared by the generic binder and the state-style compiler. */
bool parseField(const dom::ComponentFieldDesc& field, const std::string& text, std::byte* data,
                std::string& error);

/* Parses a component's authoring fields through a schema descriptor and adds
   the packed bytes to the entity. Used for components whose domain owns no
   specific UI adapter (hierarchy, render, gameplay). */
bool attachGeneric(ecs::World& world, const dom::Document& document, dom::NodeHandle node,
                   ecs::Entity entity, const dom::Component& component,
                   const dom::ComponentDesc& descriptor, std::string& error);

} // namespace resonate::ui::detail

#endif /* RESONATE_UI_COMPONENT_BIND_H */
