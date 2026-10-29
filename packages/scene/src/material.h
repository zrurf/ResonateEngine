#ifndef RESONATE_SCENE_MATERIAL_H
#define RESONATE_SCENE_MATERIAL_H

#include <string>

#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::scene
{

/* A Material's authoring fields, with the scene format's texture authoring:
   `texture="@<guid>[:<sub>]"` leaves a render::TextureRef beside the Material
   for the renderer to resolve against the products manifest, while a plain
   number keeps naming a renderer texture slot directly. */
bool applyMaterial(ecs::World& world, const dom::Document& document,
                   const dom::Component& component, const dom::ComponentDesc& descriptor,
                   ecs::Entity entity, std::string& error);

} // namespace resonate::scene

#endif /* RESONATE_SCENE_MATERIAL_H */
