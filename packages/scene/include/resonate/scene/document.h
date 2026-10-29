#ifndef RESONATE_SCENE_DOCUMENT_H
#define RESONATE_SCENE_DOCUMENT_H

/*
 * The scene adapter: a .rscene document (the shared XML DOM with the scene
 * schema's node types) instantiated into entities.
 *
 * Nesting is parenting: an Object inside an Object is its child. An Object's
 * `key` is its stable id (the WidgetKey hash applications resolve) and its
 * `name` the display Name. Components apply through the schema descriptors of
 * whatever registry the host passes, so the scene schema names no component
 * layout of its own.
 */

#include <string>

#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::scene
{

/* Instantiates the document's Scene into entities and answers the first root
   object. The registry supplies the component descriptors the document's
   component elements resolve against (a composite over the modules the scene
   names). False with a named error when the root is not a Scene, a component
   is unknown, or a value does not parse; the partial tree is unwound. */
[[nodiscard]] bool instantiate(ecs::World& world, dom::Document& document,
                               const dom::ComponentRegistry* components, ecs::Entity& out_root,
                               std::string& error);

/* The entity authored under `key`, found through the WidgetKey hashes in the
   subtree rooted at `root`. Requires the UI module's components to be
   registered (the key lives there); answers an invalid entity otherwise. */
[[nodiscard]] ecs::Entity findByKey(ecs::World& world, ecs::Entity root,
                                    std::string_view key);

/* The topmost ancestor of `entity`: the scene root a capture walks. An entity
   with no Parent answers itself. */
[[nodiscard]] ecs::Entity rootOf(ecs::World& world, ecs::Entity entity);

/* Writes the subtree rooted at `root` as a scene document: a Scene element
   wrapping the root Object, its key and name restored from SourceKey and Name,
   and the scene schema's composed components with the fields that differ from
   their defaults. Procedural entities outside the subtree are not written. */
[[nodiscard]] bool capture(ecs::World& world, ecs::Entity root, std::string& out_xml,
                           std::string& error);

} // namespace resonate::scene

#endif /* RESONATE_SCENE_DOCUMENT_H */
