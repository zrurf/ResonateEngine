#ifndef RESONATE_UI_DOCUMENT_SCHEMA_H
#define RESONATE_UI_DOCUMENT_SCHEMA_H

#include <resonate/dom/schema.h>

namespace resonate::ui
{

/* The UI document vocabulary: node types as named component compositions and
   the component names the adapter knows. It is the bridge until schemac emits
   this table from content/ui/schemas/ui.rschema. */
[[nodiscard]] const dom::SchemaRegistry& documentSchema();

} // namespace resonate::ui

#endif /* RESONATE_UI_DOCUMENT_SCHEMA_H */
