#ifndef RESONATE_UI_DOCUMENT_SCHEMA_H
#define RESONATE_UI_DOCUMENT_SCHEMA_H

#include <resonate/dom/schema.h>

namespace resonate::ui
{

/* The UI document vocabulary: node types as named component compositions,
   component names and their field layouts. Both registries read the tables
   schemac emits; a mixed content document composes this with other modules. */
[[nodiscard]] const dom::SchemaRegistry& documentSchema();
[[nodiscard]] const dom::ComponentRegistry& documentComponents();

} // namespace resonate::ui

#endif /* RESONATE_UI_DOCUMENT_SCHEMA_H */
