#include <resonate/ui/document_schema.h>

#include "resonate.ui/nodes.gen.h"

namespace resonate::ui
{
namespace
{

const dom::TableSchemaRegistry& schema()
{
    static const dom::TableSchemaRegistry registry(
        resonate::Span<const dom::NodeTypeDesc>(generated::kNodeTypes,
                                                generated::kNodeTypeCount),
        resonate::Span<const char* const>(generated::kComponents, generated::kComponentCount));
    return registry;
}

const dom::TableComponentRegistry& components()
{
    static const dom::TableComponentRegistry registry(
        resonate::Span<const dom::ComponentDesc>(generated::kComponentDescs,
                                                 generated::kComponentDescCount));
    return registry;
}

} // namespace

const dom::SchemaRegistry& documentSchema()
{
    return schema();
}

const dom::ComponentRegistry& documentComponents()
{
    return components();
}

} // namespace resonate::ui
