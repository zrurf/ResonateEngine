#ifndef RESONATE_UI_COMPONENT_BIND_H
#define RESONATE_UI_COMPONENT_BIND_H

#include <string>

#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::ui::detail
{

/* Parses a component's authoring fields through a schema descriptor and adds
   the packed bytes to the entity. Used for components whose domain owns no
   specific UI adapter (hierarchy, render, gameplay). */
bool attachGeneric(ecs::World& world, const dom::Document& document, dom::NodeHandle node,
                   ecs::Entity entity, const dom::Component& component,
                   const dom::ComponentDesc& descriptor, std::string& error);

} // namespace resonate::ui::detail

#endif /* RESONATE_UI_COMPONENT_BIND_H */
