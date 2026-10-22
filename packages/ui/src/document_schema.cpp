#include <resonate/ui/document_schema.h>

#include <cstddef>
#include <string_view>

#include "resonate.ui/nodes.gen.h"

namespace resonate::ui
{
namespace
{

std::string_view baseName(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1U);
}

class UiSchema final : public dom::SchemaRegistry
{
  public:
    [[nodiscard]] const dom::NodeTypeDesc* findNodeType(std::string_view name) const override
    {
        const std::string_view base = baseName(name);
        for (std::size_t index = 0; index < generated::kNodeTypeCount; ++index)
        {
            if (base == generated::kNodeTypes[index].name)
            {
                return &generated::kNodeTypes[index];
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool isComponent(std::string_view name) const override
    {
        const std::string_view base = baseName(name);
        for (std::size_t index = 0; index < generated::kComponentCount; ++index)
        {
            if (base == generated::kComponents[index])
            {
                return true;
            }
        }
        return false;
    }
};

} // namespace

const dom::SchemaRegistry& documentSchema()
{
    static const UiSchema schema;
    return schema;
}

} // namespace resonate::ui
