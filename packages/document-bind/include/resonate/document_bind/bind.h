#ifndef RESONATE_DOCUMENT_BIND_H
#define RESONATE_DOCUMENT_BIND_H

/*
 * The document-to-ECS bridge: a component element's authored fields, parsed
 * through a schema descriptor into the component's packed layout and attached
 * to an entity. Every adapter that instantiates a document into a world (UI,
 * scene) lands here; the package knows no domain vocabulary beyond the schema
 * descriptors.
 */

#include <cstdint>
#include <string>
#include <string_view>

#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::dom_bind
{

/* Writes one field's text into a packed component payload through its schema
   layout. The caller owns the payload's starting bytes (usually the schema's
   defaults); `data` addresses the component, `field.offset` selects the field. */
[[nodiscard]] bool parseField(const dom::ComponentFieldDesc& field, std::string_view text,
                              void* data, std::string& error);

/* Parses a component's authoring fields through a schema descriptor and adds
   the packed bytes to the entity. The component starts from the descriptor's
   defaults, so a document may name only some fields. False with a named error
   for a derived component, an unregistered one, an unknown field or a value
   that does not parse. */
[[nodiscard]] bool attachComponent(ecs::World& world, ecs::Entity entity,
                                   const dom::Document& document,
                                   const dom::Component& component,
                                   const dom::ComponentDesc& descriptor, std::string& error);

} // namespace resonate::dom_bind

#endif /* RESONATE_DOCUMENT_BIND_H */
