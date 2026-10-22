#include <resonate/ui/document_schema.h>

#include <cstddef>
#include <iterator>
#include <string_view>

namespace resonate::ui
{
namespace
{

using dom::ExposedField;
using dom::NodeTypeDesc;
using resonate::Span;
using dom::UsedComponent;

constexpr ExposedField kCanvasExposed[] = {
    {"scale", "Canvas", "scale"},
};

constexpr ExposedField kRectExposed[] = {
    {"x", "RectTransform", "x"},
    {"y", "RectTransform", "y"},
    {"width", "RectTransform", "width"},
    {"height", "RectTransform", "height"},
    {"anchor_min", "RectTransform", "anchor_min"},
    {"anchor_max", "RectTransform", "anchor_max"},
    {"pivot", "RectTransform", "pivot"},
    {"z", "RectTransform", "z"},
    {"position", "RectTransform", "position"},
};

constexpr ExposedField kBackgroundExposed[] = {
    {"color", "Background", "color"},
};

constexpr ExposedField kLayoutExposed[] = {
    {"direction", "LayoutGroup", "direction"},
    {"justify", "LayoutGroup", "justify"},
    {"align", "LayoutGroup", "align"},
    {"gap", "LayoutGroup", "gap"},
    {"padding_left", "LayoutGroup", "padding_left"},
    {"padding_top", "LayoutGroup", "padding_top"},
    {"padding_right", "LayoutGroup", "padding_right"},
    {"padding_bottom", "LayoutGroup", "padding_bottom"},
};

constexpr UsedComponent kScreenComponents[] = {
    {"Canvas", Span<const ExposedField>(kCanvasExposed, std::size(kCanvasExposed))},
};

constexpr UsedComponent kPanelComponents[] = {
    {"RectTransform", Span<const ExposedField>(kRectExposed, std::size(kRectExposed))},
    {"Background", Span<const ExposedField>(kBackgroundExposed, std::size(kBackgroundExposed))},
    {"LayoutGroup", Span<const ExposedField>(kLayoutExposed, std::size(kLayoutExposed))},
};

constexpr UsedComponent kBoxComponents[] = {
    {"RectTransform", Span<const ExposedField>(kRectExposed, std::size(kRectExposed))},
    {"Background", Span<const ExposedField>(kBackgroundExposed, std::size(kBackgroundExposed))},
};

constexpr NodeTypeDesc kNodeTypes[] = {
    {"Screen", "ui.Screen",
     Span<const UsedComponent>(kScreenComponents, std::size(kScreenComponents))},
    {"Panel", "ui.Panel",
     Span<const UsedComponent>(kPanelComponents, std::size(kPanelComponents))},
    {"Label", "ui.Label", Span<const UsedComponent>(kBoxComponents, std::size(kBoxComponents))},
    {"Box", "ui.Box", Span<const UsedComponent>(kBoxComponents, std::size(kBoxComponents))},
};

constexpr const char* kComponentNames[] = {
    "Canvas", "RectTransform", "LayoutGroup", "ComputedRect", "Background", "StyleRef",
};

std::string_view baseName(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1U);
}

class UiSchema final : public dom::SchemaRegistry
{
  public:
    [[nodiscard]] const NodeTypeDesc* findNodeType(std::string_view name) const override
    {
        const std::string_view base = baseName(name);
        for (const NodeTypeDesc& type : kNodeTypes)
        {
            if (base == type.name)
            {
                return &type;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool isComponent(std::string_view name) const override
    {
        const std::string_view base = baseName(name);
        for (const char* component : kComponentNames)
        {
            if (base == component)
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
