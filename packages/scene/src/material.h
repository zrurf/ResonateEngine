#ifndef RESONATE_SCENE_MATERIAL_H
#define RESONATE_SCENE_MATERIAL_H

#include <string>
#include <string_view>
#include <unordered_map>

#include <resonate/asset/identity.h>
#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::scene
{

/* The alias table a scene's Resources block declares: the name an object
   references ("#name") to the asset it stands for. */
using ResourceAliases = std::unordered_map<std::string, asset::AssetId>;

/* "@<guid>[:<sub>]" as an asset id, the leading '@' optional and the sub id
   defaulting to 1. False with a named error when the text is not an id. */
[[nodiscard]] bool parseAssetId(std::string_view text, asset::AssetId& out, std::string& error);

/* A Material's authoring fields, with the scene format's texture authoring:
   `texture="@<guid>[:<sub>]"` names an asset id, `texture="#<name>"` an entry
   of the scene's Resources block, and a plain number keeps naming a renderer
   texture slot. The first two leave a render::TextureRef beside the Material
   for the renderer to resolve against the products manifest; an alias also
   leaves a TextureAlias so a capture writes the name back. */
bool applyMaterial(ecs::World& world, const dom::Document& document,
                   const dom::Component& component, const dom::ComponentDesc& descriptor,
                   const ResourceAliases* aliases, ecs::Entity entity, std::string& error);

/* An Animation's authoring fields: `clip="@<guid>[:<sub>]"` or `clip="#name"`
   names the clip's asset id (the player resolves it through the products
   manifest); looping and speed go through the descriptor's own fields. */
bool applyAnimation(ecs::World& world, const dom::Document& document,
                    const dom::Component& component, const dom::ComponentDesc& descriptor,
                    const ResourceAliases* aliases, ecs::Entity entity, std::string& error);

} // namespace resonate::scene

#endif /* RESONATE_SCENE_MATERIAL_H */
