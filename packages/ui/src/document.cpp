#include <resonate/ui/document.h>

#include <resonate/ui/document_schema.h>
#include <resonate/ui/icon_registry.h>
#include <resonate/ui/image_registry.h>
#include <resonate/ui/input.h>
#include <resonate/ui/string_table.h>
#include <resonate/ui/style.h>
#include <resonate/ui/text_runtime.h>

#include "binding_internal.h"
#include "component_bind.h"
#include "glyph_run.h"
#include "state_style_internal.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <resonate/ecs/command_buffer.h>
#include <resonate/hierarchy/tree.h>

#include "resonate.ui/components.gen.h"

namespace resonate::ui
{
namespace
{

using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;

std::string_view baseName(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1U);
}

std::string nodeLabel(const dom::Document& document, dom::NodeHandle node)
{
    const dom::StringId key = document.key(node);
    if (key != dom::kInvalidString)
    {
        return std::string(document.text(key));
    }
    const dom::StringId id = document.id(node);
    if (id != dom::kInvalidString)
    {
        return std::string(document.text(id));
    }
    return "#" + std::to_string(node.index);
}

bool report(World& world, const dom::Document& document, dom::NodeHandle node,
            const std::string& message)
{
    world.report(World::Report::Error, "ui document: node '%s': %s",
                 nodeLabel(document, node).c_str(), message.c_str());
    return false;
}

bool parseFloat(std::string_view text, float& out)
{
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value))
    {
        return false;
    }
    out = value;
    return true;
}

bool parseI32(std::string_view text, std::int32_t& out)
{
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE || value < INT32_MIN ||
        value > INT32_MAX)
    {
        return false;
    }
    out = static_cast<std::int32_t>(value);
    return true;
}

bool parseU32(std::string_view text, std::uint32_t& out)
{
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE || value > UINT32_MAX)
    {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

std::vector<std::string_view> tokens(std::string_view text)
{
    std::vector<std::string_view> result;
    std::size_t index = 0;
    while (index < text.size())
    {
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0)
        {
            ++index;
        }
        const std::size_t begin = index;
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0)
        {
            ++index;
        }
        if (index > begin)
        {
            result.push_back(text.substr(begin, index - begin));
        }
    }
    return result;
}

bool parseVec2(std::string_view text, Vec2& out)
{
    const auto parts = tokens(text);
    if (parts.size() != 2U || !parseFloat(parts[0], out.x) || !parseFloat(parts[1], out.y))
    {
        return false;
    }
    return true;
}

bool parseVec4(std::string_view text, Vec4& out)
{
    const auto parts = tokens(text);
    if (parts.size() != 4U || !parseFloat(parts[0], out.x) || !parseFloat(parts[1], out.y) ||
        !parseFloat(parts[2], out.z) || !parseFloat(parts[3], out.w))
    {
        return false;
    }
    return true;
}

bool applyRect(const dom::Document& document, const dom::Component& component, RectTransform& rect,
               std::string& error)
{
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "x")
        {
            if (!parseFloat(field.value, rect.x))
            {
                error = "RectTransform.x is not a number";
                return false;
            }
        }
        else if (name == "y")
        {
            if (!parseFloat(field.value, rect.y))
            {
                error = "RectTransform.y is not a number";
                return false;
            }
        }
        else if (name == "width")
        {
            if (!parseFloat(field.value, rect.width))
            {
                error = "RectTransform.width is not a number";
                return false;
            }
        }
        else if (name == "height")
        {
            if (!parseFloat(field.value, rect.height))
            {
                error = "RectTransform.height is not a number";
                return false;
            }
        }
        else if (name == "anchor_min")
        {
            if (!parseVec2(field.value, rect.anchor_min))
            {
                error = "RectTransform.anchor_min is not a vec2";
                return false;
            }
        }
        else if (name == "anchor_max")
        {
            if (!parseVec2(field.value, rect.anchor_max))
            {
                error = "RectTransform.anchor_max is not a vec2";
                return false;
            }
        }
        else if (name == "pivot")
        {
            if (!parseVec2(field.value, rect.pivot))
            {
                error = "RectTransform.pivot is not a vec2";
                return false;
            }
        }
        else if (name == "z")
        {
            if (!parseI32(field.value, rect.z))
            {
                error = "RectTransform.z is not an i32";
                return false;
            }
        }
        else if (name == "position")
        {
            if (field.value == "Relative")
            {
                rect.position = PositionMode::Relative;
            }
            else if (field.value == "Absolute")
            {
                rect.position = PositionMode::Absolute;
            }
            else
            {
                error = "RectTransform.position is Relative or Absolute";
                return false;
            }
        }
        else
        {
            error = "unknown RectTransform field '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

bool applyCanvas(const dom::Document& document, const dom::Component& component, Canvas& canvas,
                 std::string& error)
{
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "mode")
        {
            if (field.value == "Retained")
            {
                canvas.mode = UpdateMode::Retained;
            }
            else if (field.value == "Immediate")
            {
                canvas.mode = UpdateMode::Immediate;
            }
            else
            {
                error = "Canvas.mode is Retained or Immediate";
                return false;
            }
        }
        else if (name == "scale")
        {
            if (!parseFloat(field.value, canvas.scale))
            {
                error = "Canvas.scale is not a number";
                return false;
            }
        }
        else
        {
            error = "unknown Canvas field '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

bool applyLayoutGroup(const dom::Document& document, const dom::Component& component,
                      LayoutGroup& layout, std::string& error)
{
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "direction")
        {
            if (field.value == "Row")
            {
                layout.direction = FlexDirection::Row;
            }
            else if (field.value == "Column")
            {
                layout.direction = FlexDirection::Column;
            }
            else
            {
                error = "LayoutGroup.direction is Row or Column";
                return false;
            }
        }
        else if (name == "justify")
        {
            if (field.value == "Start")
            {
                layout.justify = Justify::Start;
            }
            else if (field.value == "Center")
            {
                layout.justify = Justify::Center;
            }
            else if (field.value == "End")
            {
                layout.justify = Justify::End;
            }
            else if (field.value == "SpaceBetween")
            {
                layout.justify = Justify::SpaceBetween;
            }
            else
            {
                error = "LayoutGroup.justify is Start, Center, End or SpaceBetween";
                return false;
            }
        }
        else if (name == "align")
        {
            if (field.value == "Start")
            {
                layout.align = Align::Start;
            }
            else if (field.value == "Center")
            {
                layout.align = Align::Center;
            }
            else if (field.value == "End")
            {
                layout.align = Align::End;
            }
            else if (field.value == "Stretch")
            {
                layout.align = Align::Stretch;
            }
            else
            {
                error = "LayoutGroup.align is Start, Center, End or Stretch";
                return false;
            }
        }
        else if (name == "gap")
        {
            if (!parseFloat(field.value, layout.gap))
            {
                error = "LayoutGroup.gap is not a number";
                return false;
            }
        }
        else if (name == "padding_left")
        {
            if (!parseFloat(field.value, layout.padding_left))
            {
                error = "LayoutGroup.padding_left is not a number";
                return false;
            }
        }
        else if (name == "padding_top")
        {
            if (!parseFloat(field.value, layout.padding_top))
            {
                error = "LayoutGroup.padding_top is not a number";
                return false;
            }
        }
        else if (name == "padding_right")
        {
            if (!parseFloat(field.value, layout.padding_right))
            {
                error = "LayoutGroup.padding_right is not a number";
                return false;
            }
        }
        else if (name == "padding_bottom")
        {
            if (!parseFloat(field.value, layout.padding_bottom))
            {
                error = "LayoutGroup.padding_bottom is not a number";
                return false;
            }
        }
        else if (name == "wrap")
        {
            if (field.value != "true" && field.value != "false")
            {
                error = "LayoutGroup.wrap is not a bool";
                return false;
            }
            layout.wrap = field.value == "true";
        }
        else
        {
            error = "unknown LayoutGroup field '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

bool applyLayoutItem(const dom::Document& document, const dom::Component& component,
                     LayoutItem& item, std::string& error)
{
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "grow")
        {
            if (!parseFloat(field.value, item.grow) || item.grow < 0.0F)
            {
                error = "LayoutItem.grow is not a non-negative number";
                return false;
            }
        }
        else if (name == "shrink")
        {
            if (!parseFloat(field.value, item.shrink) || item.shrink < 0.0F)
            {
                error = "LayoutItem.shrink is not a non-negative number";
                return false;
            }
        }
        else if (name == "align_self")
        {
            if (field.value == "Auto")
            {
                item.align_self = SelfAlign::Auto;
            }
            else if (field.value == "Start")
            {
                item.align_self = SelfAlign::Start;
            }
            else if (field.value == "Center")
            {
                item.align_self = SelfAlign::Center;
            }
            else if (field.value == "End")
            {
                item.align_self = SelfAlign::End;
            }
            else if (field.value == "Stretch")
            {
                item.align_self = SelfAlign::Stretch;
            }
            else
            {
                error = "LayoutItem.align_self is Auto, Start, Center, End or Stretch";
                return false;
            }
        }
        else if (name == "margin_left")
        {
            if (!parseFloat(field.value, item.margin_left) || item.margin_left < 0.0F)
            {
                error = "LayoutItem.margin_left is not a non-negative number";
                return false;
            }
        }
        else if (name == "margin_top")
        {
            if (!parseFloat(field.value, item.margin_top) || item.margin_top < 0.0F)
            {
                error = "LayoutItem.margin_top is not a non-negative number";
                return false;
            }
        }
        else if (name == "margin_right")
        {
            if (!parseFloat(field.value, item.margin_right) || item.margin_right < 0.0F)
            {
                error = "LayoutItem.margin_right is not a non-negative number";
                return false;
            }
        }
        else if (name == "margin_bottom")
        {
            if (!parseFloat(field.value, item.margin_bottom) || item.margin_bottom < 0.0F)
            {
                error = "LayoutItem.margin_bottom is not a non-negative number";
                return false;
            }
        }
        else if (name == "min_width")
        {
            if (!parseFloat(field.value, item.min_width) || item.min_width < 0.0F)
            {
                error = "LayoutItem.min_width is not a non-negative number";
                return false;
            }
        }
        else if (name == "min_height")
        {
            if (!parseFloat(field.value, item.min_height) || item.min_height < 0.0F)
            {
                error = "LayoutItem.min_height is not a non-negative number";
                return false;
            }
        }
        else if (name == "max_width")
        {
            if (!parseFloat(field.value, item.max_width) || item.max_width < 0.0F)
            {
                error = "LayoutItem.max_width is not a non-negative number";
                return false;
            }
        }
        else if (name == "max_height")
        {
            if (!parseFloat(field.value, item.max_height) || item.max_height < 0.0F)
            {
                error = "LayoutItem.max_height is not a non-negative number";
                return false;
            }
        }
        else
        {
            error = "unknown LayoutItem field '" + std::string(name) + "'";
            return false;
        }
    }
    if ((item.min_width > 0.0F && item.max_width > 0.0F && item.min_width > item.max_width) ||
        (item.min_height > 0.0F && item.max_height > 0.0F && item.min_height > item.max_height))
    {
        error = "LayoutItem min exceeds max";
        return false;
    }
    return true;
}

bool applyBackground(const dom::Document& document, const dom::Component& component,
                     Background& background, std::string& error)
{
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "color")
        {
            if (!parseVec4(field.value, background.color))
            {
                error = "Background.color is not a vec4";
                return false;
            }
        }
        else if (name == "border_color")
        {
            if (!parseVec4(field.value, background.border_color))
            {
                error = "Background.border_color is not a vec4";
                return false;
            }
        }
        else if (name == "border_width")
        {
            if (!parseFloat(field.value, background.border_width) || background.border_width < 0.0F)
            {
                error = "Background.border_width is not a non-negative number";
                return false;
            }
        }
        else if (name == "radius")
        {
            if (!parseFloat(field.value, background.radius) || background.radius < 0.0F)
            {
                error = "Background.radius is not a non-negative number";
                return false;
            }
        }
        else
        {
            error = "unknown Background field '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

bool applyImage(const dom::Document& document, const dom::Component& component, Image& image,
                std::string& error)
{
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "texture")
        {
            if (!parseU32(field.value, image.texture))
            {
                error = "Image.texture is not a u32";
                return false;
            }
        }
        else if (name == "uv_min")
        {
            if (!parseVec2(field.value, image.uv_min))
            {
                error = "Image.uv_min is not a vec2";
                return false;
            }
        }
        else if (name == "uv_max")
        {
            if (!parseVec2(field.value, image.uv_max))
            {
                error = "Image.uv_max is not a vec2";
                return false;
            }
        }
        else if (name == "tint")
        {
            if (!parseVec4(field.value, image.tint))
            {
                error = "Image.tint is not a vec4";
                return false;
            }
        }
        else if (name == "slice")
        {
            if (!parseVec4(field.value, image.slice))
            {
                error = "Image.slice is not a vec4";
                return false;
            }
        }
        else
        {
            error = "unknown Image field '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

bool applyStyleRef(const dom::Document& document, const dom::Component& component,
                   StyleRef& style, std::string& error){
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "style")
        {
            if (!parseU32(field.value, style.style))
            {
                error = "StyleRef.style is not a u32";
                return false;
            }
        }
        else
        {
            error = "unknown StyleRef field '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

bool applyDataContext(const dom::Document& document, const dom::Component& component,
                       DataContext& context, std::string& error)
{
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "health")
        {
            if (!parseFloat(field.value, context.health))
            {
                error = "DataContext.health is not a number";
                return false;
            }
        }
        else if (name == "max_health")
        {
            if (!parseFloat(field.value, context.max_health))
            {
                error = "DataContext.max_health is not a number";
                return false;
            }
        }
        else if (name == "ammo")
        {
            if (!parseFloat(field.value, context.ammo))
            {
                error = "DataContext.ammo is not a number";
                return false;
            }
        }
        else if (name == "max_ammo")
        {
            if (!parseFloat(field.value, context.max_ammo))
            {
                error = "DataContext.max_ammo is not a number";
                return false;
            }
        }
        else
        {
            error = "unknown DataContext field '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

bool applyInput(const dom::Document& document, const dom::Component& component, Input& input,
                std::string& error)
{
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "state")
        {
            if (field.value == "Normal")
            {
                input.state = WidgetState::Normal;
            }
            else if (field.value == "Hovered")
            {
                input.state = WidgetState::Hovered;
            }
            else if (field.value == "Pressed")
            {
                input.state = WidgetState::Pressed;
            }
            else if (field.value == "Disabled")
            {
                input.state = WidgetState::Disabled;
            }
            else if (field.value == "Focused")
            {
                input.state = WidgetState::Focused;
            }
            else
            {
                error = "Input.state is not a WidgetState";
                return false;
            }
        }
        else if (name == "focusable" || name == "activated")
        {
            if (field.value != "true" && field.value != "false")
            {
                error = "Input." + std::string(name) + " is not a bool";
                return false;
            }
            const bool value = field.value == "true";
            if (name == "focusable")
            {
                input.focusable = value;
            }
            else
            {
                input.activated = value;
            }
        }
        else
        {
            error = "unknown Input field '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

bool applyText(const dom::Document& document, const dom::Component& component, Text& text,
               std::string& error)
{
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "font_size")
        {
            if (!parseFloat(field.value, text.font_size) || !(text.font_size > 0.0F))
            {
                error = "Text.font_size is not a positive number";
                return false;
            }
        }
        else if (name == "font_weight")
        {
            std::uint32_t weight = 0U;
            if (!parseU32(field.value, weight) || weight < 1U || weight > 1000U)
            {
                error = "Text.font_weight is not a weight between 1 and 1000";
                return false;
            }
            text.font_weight = static_cast<std::uint16_t>(weight);
        }
        else if (name == "text_color")
        {
            if (!parseVec4(field.value, text.text_color))
            {
                error = "Text.text_color is not a vec4";
                return false;
            }
        }
        else if (name == "align")
        {
            if (field.value == "Start")
            {
                text.align = TextAlign::Start;
            }
            else if (field.value == "Center")
            {
                text.align = TextAlign::Center;
            }
            else if (field.value == "End")
            {
                text.align = TextAlign::End;
            }
            else
            {
                error = "Text.align is Start, Center or End";
                return false;
            }
        }
        else if (name == "wrap")
        {
            if (field.value != "true" && field.value != "false")
            {
                error = "Text.wrap is not a bool";
                return false;
            }
            text.wrap = field.value == "true";
        }
        else if (name == "v_align")
        {
            if (field.value == "Top")
            {
                text.v_align = VAlign::Top;
            }
            else if (field.value == "Middle")
            {
                text.v_align = VAlign::Middle;
            }
            else if (field.value == "Bottom")
            {
                text.v_align = VAlign::Bottom;
            }
            else
            {
                error = "Text.v_align is Top, Middle or Bottom";
                return false;
            }
        }
        else if (name == "padding_left")
        {
            if (!parseFloat(field.value, text.padding_left) || text.padding_left < 0.0F)
            {
                error = "Text.padding_left is not a non-negative number";
                return false;
            }
        }
        else if (name == "content")
        {
            error = "Text.content is written by the adapter and cannot be authored";
            return false;
        }
        else
        {
            error = "unknown Text field '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

struct PendingRun
{
    Entity entity{};
    std::vector<detail::GlyphQuad> quads;
    float advance = 0.0F;
};

struct BindingContext
{
    Entity canvas{};
    ecs::ComponentIndex data_component = ecs::kInvalidComponent;
    std::vector<detail::BindingRecord> records;
    dom::StringId text_property = dom::kInvalidString;
    dom::StringId image_property = dom::kInvalidString;
    dom::StringId icon_property = dom::kInvalidString;
    std::unique_ptr<detail::TextAtlasBuilder> owned_atlas;
    detail::TextAtlasBuilder* atlas = nullptr;
    TextRuntime* runtime = nullptr;
    const StringTable* strings = nullptr;
    const ImageRegistry* images = nullptr;
    const IconRegistry* icons = nullptr;
    std::uint32_t page_revision = 1U;
    std::vector<PendingRun> runs;
};

bool collectBindings(World& world, const dom::Document& document, dom::NodeHandle node,
                     Entity entity, BindingContext& context)
{
    for (const dom::Field& property : document.properties(node))
    {
        const std::string_view name = document.text(property.name);
        if (name.rfind("bind-", 0U) != 0U)
        {
            continue;
        }

        const std::string_view target = name.substr(5U);
        std::uint32_t target_offset = 0;
        if (target == "x")
        {
            target_offset = offsetof(RectTransform, x);
        }
        else if (target == "y")
        {
            target_offset = offsetof(RectTransform, y);
        }
        else if (target == "width")
        {
            target_offset = offsetof(RectTransform, width);
        }
        else if (target == "height")
        {
            target_offset = offsetof(RectTransform, height);
        }
        else
        {
            return report(world, document, node,
                          "unknown binding target '" + std::string(target) + "'");
        }
        if (world.get<RectTransform>(entity) == nullptr)
        {
            return report(world, document, node,
                          "bind-" + std::string(target) + " needs a RectTransform");
        }
        for (const dom::Component& component : document.components(node))
        {
            if (baseName(document.text(component.type)) != "RectTransform")
            {
                continue;
            }
            for (const dom::Field& field : component.fields)
            {
                if (document.text(field.name) == target)
                {
                    return report(world, document, node,
                                  "both " + std::string(target) + " and bind-" +
                                      std::string(target) + " are authored");
                }
            }
        }

        const std::string_view expression = property.value;
        const std::size_t dot = expression.find('.');
        if (dot == std::string_view::npos || dot == 0U || dot + 1U >= expression.size())
        {
            return report(world, document, node, "a binding expression is Component.field");
        }
        const std::string_view component_name = expression.substr(0U, dot);
        const std::string_view field_name = expression.substr(dot + 1U);
        if (baseName(component_name) != "DataContext")
        {
            return report(world, document, node,
                          "only DataContext fields can be bound in this version");
        }
        std::uint32_t source_offset = 0;
        if (field_name == "health")
        {
            source_offset = offsetof(DataContext, health);
        }
        else if (field_name == "max_health")
        {
            source_offset = offsetof(DataContext, max_health);
        }
        else if (field_name == "ammo")
        {
            source_offset = offsetof(DataContext, ammo);
        }
        else if (field_name == "max_ammo")
        {
            source_offset = offsetof(DataContext, max_ammo);
        }
        else
        {
            return report(world, document, node,
                          "unknown DataContext field '" + std::string(field_name) + "'");
        }
        if (context.data_component == ecs::kInvalidComponent || !context.canvas.valid())
        {
            return report(world, document, node, "bindings need a DataContext on the canvas");
        }
        if (ecs::ComponentTraits<RectTransform>::index == ecs::kInvalidComponent)
        {
            return report(world, document, node, "RectTransform is not registered");
        }

        detail::BindingRecord record;
        record.source = context.canvas;
        record.target = entity;
        record.source_component = context.data_component;
        record.target_component = ecs::ComponentTraits<RectTransform>::index;
        record.source_offset = source_offset;
        record.target_offset = target_offset;
        record.kind = 0U;
        context.records.push_back(record);
    }
    return true;
}

bool buildTextRun(World& world, const dom::Document& document, dom::NodeHandle node,
                  Entity entity, BindingContext& context)
{
    Text* text = world.get<Text>(entity);
    if (text == nullptr)
    {
        return true;
    }

    if (context.text_property != dom::kInvalidString)
    {
        if (const dom::Field* property = document.property(node, context.text_property))
        {
            std::string_view content = property->value;
            std::string resolved;
            if (!content.empty() && content.front() == '#')
            {
                const std::string_view key = content.substr(1U);
                const std::string* found =
                    context.strings != nullptr ? context.strings->find(key) : nullptr;
                if (found == nullptr)
                {
                    world.report(World::Report::Warning,
                                 "ui document: node '%s': string key '%s' is missing",
                                 nodeLabel(document, node).c_str(), std::string(key).c_str());
                    resolved.assign(key);
                }
                else
                {
                    resolved = *found;
                }
                content = resolved;
            }
            if (!content.empty())
            {
                const ecs::BlobHandle blob =
                    world.createBlob(entity, content.size(), content.data());
                if (!blob.valid())
                {
                    return report(world, document, node, "the text bytes could not be attached");
                }
                text->content = blob;
            }
        }
    }
    if (!text->content.valid() || context.atlas == nullptr)
    {
        return true;
    }
    const auto* bytes = static_cast<const char*>(world.blobData(text->content));
    const std::size_t size = world.blobSize(text->content);
    if (bytes == nullptr || size == 0U)
    {
        return true;
    }

    const RectTransform* rect = world.get<RectTransform>(entity);
    /* The left padding shrinks the alignment span; the display list shifts the
       ink by it. */
    const float width =
        rect != nullptr ? std::fmax(0.0F, rect->width - text->padding_left) : 0.0F;
    std::vector<detail::GlyphQuad> glyphs;
    std::string error;
    float advance = 0.0F;
    if (!context.atlas->buildRun(std::string_view(bytes, size), text->font_size,
                                 static_cast<float>(text->font_weight), width,
                                 static_cast<std::uint32_t>(text->align), text->wrap, glyphs,
                                 advance, error))
    {
        return report(world, document, node, "text: " + error);
    }
    if (glyphs.empty())
    {
        return true;
    }
    context.runs.push_back(PendingRun{entity, std::move(glyphs), advance});
    return true;
}

const dom::ComponentDesc* findDescriptor(std::string_view name,
                                         const dom::ComponentRegistry* extra)
{
    if (const dom::ComponentDesc* descriptor = documentComponents().find(name))
    {
        return descriptor;
    }
    return extra != nullptr ? extra->find(name) : nullptr;
}

const dom::ComponentFieldDesc* findFieldDesc(const dom::ComponentDesc& descriptor,
                                             std::string_view name)
{
    for (const dom::ComponentFieldDesc& field : descriptor.fields)
    {
        if (field.name != nullptr && name == field.name)
        {
            return &field;
        }
    }
    return nullptr;
}

/* Compiles the difference between the normal and each state's resolved
   declarations into records on the widget. Only nodes with Input have states,
   and a state rule may only change a field the normal declarations also set,
   so restoring the normal value needs no defaults. */
bool collectStateStyles(World& world, const dom::Document& document, dom::NodeHandle node,
                        Entity entity, const StyleTable& styles,
                        const dom::ComponentRegistry* component_registry)
{
    if (!world.has<Input>(entity))
    {
        return true;
    }

    const std::vector<dom::Component> normal = styles.resolve(document, node, 0U);
    std::vector<detail::StateStyleRecord> records;
    for (std::uint32_t state = 1U; state <= 4U; ++state)
    {
        for (const dom::Component& styled : styles.resolve(document, node, state))
        {
            const std::string_view component_name = document.text(styled.type);
            bool has_normal = false;
            for (const dom::Component& candidate : normal)
            {
                if (candidate.type == styled.type)
                {
                    has_normal = true;
                    break;
                }
            }
            if (!has_normal)
            {
                return report(world, document, node,
                              "a state rule may not add component '" +
                                  std::string(component_name) + "'");
            }

            const dom::ComponentDesc* descriptor =
                findDescriptor(component_name, component_registry);
            if (descriptor == nullptr || descriptor->qualified_name == nullptr)
            {
                return report(world, document, node,
                              "state rules need a registered component for '" +
                                  std::string(component_name) + "'");
            }
            if (descriptor->derived != 0U)
            {
                return report(world, document, node,
                              "a state rule may not declare derived component '" +
                                  std::string(component_name) + "'");
            }
            const ecs::ComponentIndex index = world.findComponent(descriptor->qualified_name);
            if (index == ecs::kInvalidComponent)
            {
                return report(world, document, node,
                              "component '" + std::string(descriptor->qualified_name) +
                                  "' is not registered");
            }

            for (const dom::Field& styled_field : styled.fields)
            {
                const std::string_view field_name = document.text(styled_field.name);
                const dom::Field* normal_field = nullptr;
                for (const dom::Component& candidate : normal)
                {
                    if (candidate.type != styled.type)
                    {
                        continue;
                    }
                    for (const dom::Field& field : candidate.fields)
                    {
                        if (field.name == styled_field.name)
                        {
                            normal_field = &field;
                            break;
                        }
                    }
                    if (normal_field != nullptr)
                    {
                        break;
                    }
                }
                if (normal_field == nullptr)
                {
                    return report(world, document, node,
                                  "a state rule sets " + std::string(component_name) + "." +
                                      std::string(field_name) +
                                      " but the normal style does not");
                }
                if (normal_field->value == styled_field.value)
                {
                    continue;
                }

                const dom::ComponentFieldDesc* layout = findFieldDesc(*descriptor, field_name);
                if (layout == nullptr)
                {
                    return report(world, document, node,
                                  "unknown field '" + std::string(field_name) + "' on " +
                                      std::string(component_name));
                }
                if (layout->size == 0U || layout->size > detail::kStateStyleValueSize)
                {
                    return report(world, document, node,
                                  "field " + std::string(component_name) + "." +
                                      std::string(field_name) +
                                      " is too large for a state style");
                }

                detail::StateStyleRecord value;
                value.component = index;
                value.offset = layout->offset;
                value.size = layout->size;
                /* The record keeps the value alone; parse at offset zero, or a
                   field past the first would write outside value[16]. */
                dom::ComponentFieldDesc field_layout = *layout;
                field_layout.offset = 0;
                std::string error;
                if (!detail::parseField(field_layout, normal_field->value, value.value, error))
                {
                    return report(world, document, node,
                                  std::string(component_name) + "." + std::string(field_name) +
                                      ": " + error);
                }
                value.state = 0U;
                bool has_restore = false;
                for (const detail::StateStyleRecord& record : records)
                {
                    if (record.state == 0U && record.component == value.component &&
                        record.offset == value.offset)
                    {
                        has_restore = true;
                        break;
                    }
                }
                if (!has_restore)
                {
                    records.push_back(value);
                }
                if (!detail::parseField(field_layout, styled_field.value, value.value, error))
                {
                    return report(world, document, node,
                                  std::string(component_name) + "." + std::string(field_name) +
                                      ": " + error);
                }
                value.state = state;
                records.push_back(value);
            }
        }
    }

    if (records.empty())
    {
        return true;
    }
    const ecs::BlobHandle blob = world.createBlob(
        entity, records.size() * sizeof(detail::StateStyleRecord), records.data());
    if (!blob.valid() || world.add(entity, StateStyles{blob}) == nullptr)
    {
        return report(world, document, node, "state-style records could not be attached");
    }
    return true;
}

bool applyComponents(World& world, const dom::Document& document, dom::NodeHandle node,
                     Entity entity, const std::vector<dom::Component>& components,
                     BindingContext& bindings, const dom::ComponentRegistry* component_registry)
{
    const dom::Component* computed = nullptr;
    const dom::Component* authored_derived = nullptr;
    for (const dom::Component& component : components)
    {
        const std::string_view name = baseName(document.text(component.type));
        if (name == "ComputedRect")
        {
            computed = &component;
            continue;
        }
        if (name == "Bindings" || name == "PointerState" || name == "StateStyles" ||
            name == "Glyphs" || name == "TextAtlasPage")
        {
            authored_derived = &component;
            continue;
        }

        std::string error;
        bool applied = false;
        if (name == "RectTransform")
        {
            RectTransform value;
            applied = applyRect(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else if (name == "Canvas")
        {
            Canvas value;
            applied = applyCanvas(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else if (name == "LayoutGroup")
        {
            LayoutGroup value;
            applied = applyLayoutGroup(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else if (name == "LayoutItem")
        {
            LayoutItem value;
            applied = applyLayoutItem(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else if (name == "Background")
        {
            Background value;
            applied = applyBackground(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else if (name == "Image")
        {
            Image value;
            applied = applyImage(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
            /* A `path` authoring field names a registered file; the resolved id
               overrides the authored texture. */
            if (applied && document.property(node, bindings.image_property) != nullptr)
            {
                const dom::Field* property = document.property(node, bindings.image_property);
                const std::uint32_t id =
                    bindings.images != nullptr ? bindings.images->find(property->value)
                                               : ImageRegistry::kUnregistered;
                if (id == ImageRegistry::kUnregistered)
                {
                    world.report(World::Report::Warning,
                                 "ui document: node '%s': image path '%s' is not registered",
                                 nodeLabel(document, node).c_str(),
                                 std::string(property->value).c_str());
                }
                else if (Image* image = world.get<Image>(entity); image != nullptr)
                {
                    image->texture = id;
                }
            }
            /* An `icon` authoring field names an atlas entry; the registry
               supplies both the atlas texture and the sub-rect. */
            if (applied && document.property(node, bindings.icon_property) != nullptr)
            {
                const dom::Field* property = document.property(node, bindings.icon_property);
                const IconRegistry::Icon* icon =
                    bindings.icons != nullptr ? bindings.icons->find(property->value) : nullptr;
                if (icon == nullptr)
                {
                    world.report(World::Report::Warning,
                                 "ui document: node '%s': icon '%s' is not registered",
                                 nodeLabel(document, node).c_str(),
                                 std::string(property->value).c_str());
                }
                else if (Image* image = world.get<Image>(entity); image != nullptr)
                {
                    image->texture = bindings.icons->texture();
                    image->uv_min = icon->uv_min;
                    image->uv_max = icon->uv_max;
                }
            }
        }
        else if (name == "StyleRef")
        {
            StyleRef value;
            applied = applyStyleRef(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else if (name == "DataContext")
        {
            DataContext value;
            applied = applyDataContext(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else if (name == "Input")
        {
            Input value;
            applied = applyInput(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else if (name == "Text")
        {
            Text value;
            applied = applyText(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else
        {
            const dom::ComponentDesc* descriptor =
                component_registry != nullptr ? component_registry->find(name) : nullptr;
            if (descriptor == nullptr)
            {
                error = "unknown component '" + std::string(document.text(component.type)) + "'";
            }
            else
            {
                applied = detail::attachGeneric(world, document, node, entity, component,
                                                *descriptor, error);
            }
        }
        if (!applied)
        {
            if (error.empty())
            {
                error = "component could not be attached";
            }
            return report(world, document, node, error);
        }
    }

    if (computed != nullptr)
    {
        return report(world, document, node,
                      "ComputedRect is derived by layout and cannot be authored");
    }
    if (authored_derived != nullptr)
    {
        return report(world, document, node,
                      "Bindings, PointerState, StateStyles, Glyphs and TextAtlasPage are derived "
                      "and cannot be authored");
    }
    if (world.add(entity, ComputedRect{}) == nullptr)
    {
        return report(world, document, node, "ComputedRect could not be attached");
    }
    if (entity == bindings.canvas)
    {
        bindings.data_component = ecs::ComponentTraits<DataContext>::index;
    }
    return collectBindings(world, document, node, entity, bindings);
}

Entity instantiateNode(World& world, CommandBuffer& commands, const dom::Document& document,
                       dom::NodeHandle node, const StyleTable& styles, BindingContext& bindings,
                       const dom::ComponentRegistry* component_registry,
                       std::vector<Entity>& created)
{
    const Entity entity = world.create();
    if (!entity.valid())
    {
        report(world, document, node, "an entity could not be created");
        return Entity{};
    }
    created.push_back(entity);
    if (!bindings.canvas.valid())
    {
        bindings.canvas = entity;
    }

    /* The document's key survives as a hash, so application code can reach the
       widget after the document is dropped. */
    if (const dom::StringId key = document.key(node); key != dom::kInvalidString)
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const char character : document.text(key))
        {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
            hash *= 1099511628211ULL;
        }
        if (world.add(entity, WidgetKey{hash}) == nullptr)
        {
            report(world, document, node, "the widget key could not be attached");
            return Entity{};
        }
    }

    if (!applyComponents(world, document, node, entity, styles.resolve(document, node), bindings,
                         component_registry) ||
        !collectStateStyles(world, document, node, entity, styles, component_registry))
    {
        return Entity{};
    }
    Input* input = world.get<Input>(entity);
    if (input != nullptr && !input->focusable)
    {
        input->state = WidgetState::Disabled;
        world.markChanged<Input>(entity);
        detail::applyStateStyles(world, entity, static_cast<std::uint32_t>(WidgetState::Disabled));
    }
    if (!buildTextRun(world, document, node, entity, bindings))
    {
        return Entity{};
    }

    for (dom::NodeHandle child = document.firstChild(node); child.valid();
         child = document.nextSibling(child))
    {
        if (isStyleNode(document, child))
        {
            continue;
        }
        const Entity child_entity =
            instantiateNode(world, commands, document, child, styles, bindings,
                            component_registry, created);
        if (!child_entity.valid())
        {
            return Entity{};
        }
        resonate::hierarchy::setParent(commands, child_entity, entity);
    }
    return entity;
}

Entity instantiateConfigured(World& world, dom::Document& document,
                             const dom::ComponentRegistry* component_registry,
                             BindingContext& bindings)
{
    const dom::NodeHandle root = document.root();
    if (!root.valid())
    {
        world.report(World::Report::Error, "ui document: the document has no root node");
        return Entity{};
    }
    if (isStyleNode(document, root))
    {
        world.report(World::Report::Error, "ui document: the root cannot be a style sheet");
        return Entity{};
    }

    StyleTable styles;
    std::vector<dom::Diagnostic> diagnostics;
    if (!buildStyles(document, styles, diagnostics))
    {
        for (const dom::Diagnostic& diagnostic : diagnostics)
        {
            world.report(World::Report::Error, "ui style: %s", diagnostic.message.c_str());
        }
        return Entity{};
    }

    CommandBuffer commands(world);
    std::vector<Entity> created;
    bindings.text_property = document.intern("text");
    bindings.image_property = document.intern("path");
    bindings.icon_property = document.intern("icon");
    const Entity entity = instantiateNode(world, commands, document, root, styles, bindings,
                                          component_registry, created);
    if (!entity.valid())
    {
        for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator)
        {
            world.destroy(*iterator);
        }
        return Entity{};
    }
    if (world.get<Canvas>(entity) == nullptr)
    {
        report(world, document, root, "the root node must carry Canvas");
        for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator)
        {
            world.destroy(*iterator);
        }
        return Entity{};
    }
    for (PendingRun& run : bindings.runs)
    {
        for (detail::GlyphQuad& glyph : run.quads)
        {
            if (!bindings.atlas->updateQuad(glyph))
            {
                report(world, document, root, "a glyph is missing from the atlas");
                for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator)
                {
                    world.destroy(*iterator);
                }
                return Entity{};
            }
        }
        const ecs::BlobHandle records = world.createBlob(
            run.entity, run.quads.size() * sizeof(detail::GlyphQuad), run.quads.data());
        if (!records.valid() || world.add(run.entity, Glyphs{records}) == nullptr)
        {
            report(world, document, root, "glyph quads could not be attached");
            for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator)
            {
                world.destroy(*iterator);
            }
            return Entity{};
        }

        float minimum_x = run.quads[0].x;
        float minimum_y = run.quads[0].y;
        float maximum_x = run.quads[0].x + run.quads[0].width;
        float maximum_y = run.quads[0].y + run.quads[0].height;
        for (const detail::GlyphQuad& glyph : run.quads)
        {
            minimum_x = std::fmin(minimum_x, glyph.x);
            minimum_y = std::fmin(minimum_y, glyph.y);
            maximum_x = std::fmax(maximum_x, glyph.x + glyph.width);
            maximum_y = std::fmax(maximum_y, glyph.y + glyph.height);
        }
        if (world.add(run.entity,
                      TextMetrics{maximum_x - minimum_x, maximum_y - minimum_y,
                                  run.advance}) == nullptr)
        {
            report(world, document, root, "text metrics could not be attached");
            for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator)
            {
                world.destroy(*iterator);
            }
            return Entity{};
        }
    }
    if (bindings.runtime != nullptr)
    {
        TextRuntimeAccess::attach(*bindings.runtime, entity);
        if (world.add(entity, TextRuntimeRef{bindings.runtime}) == nullptr)
        {
            return Entity{};
        }
        bindings.page_revision = TextRuntimeAccess::revision(*bindings.runtime);
    }
    if (world.add(entity, PointerState{}) == nullptr ||
        world.add(entity, ui::DragState{}) == nullptr)
    {
        report(world, document, root, "PointerState could not be attached");
        for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator)
        {
            world.destroy(*iterator);
        }
        return Entity{};
    }
    if (bindings.atlas != nullptr && bindings.atlas->used())
    {
        const std::vector<std::uint8_t>& pixels = bindings.atlas->pixels();
        const ecs::BlobHandle page = world.createBlob(entity, pixels.size(), pixels.data());
        if (!page.valid() ||
            world.add(entity, TextAtlasPage{bindings.atlas->width(), bindings.atlas->height(),
                                            bindings.page_revision, page}) == nullptr)
        {
            report(world, document, root, "the font atlas page could not be attached");
            for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator)
            {
                world.destroy(*iterator);
            }
            return Entity{};
        }
    }
    if (!bindings.records.empty())
    {
        const ecs::BlobHandle records =
            world.createBlob(entity, bindings.records.size() * sizeof(detail::BindingRecord),
                             bindings.records.data());
        if (!records.valid() || world.add(entity, Bindings{records}) == nullptr)
        {
            report(world, document, root, "binding records could not be attached");
            for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator)
            {
                world.destroy(*iterator);
            }
            return Entity{};
        }
    }
    world.play(commands);
    return entity;
}

} // namespace

Entity instantiate(World& world, dom::Document& document,
                   const dom::ComponentRegistry* component_registry,
                   std::vector<std::shared_ptr<const text::Font>> fonts,
                   const StringTable* strings, const ImageRegistry* images,
                   const IconRegistry* icons)
{
    /* The UI's own schema is the fallback registry: a widget component only the
       generated table knows (ScrollView, a view's own extensions) must attach
       without the host passing a composed registry. */
    if (component_registry == nullptr)
    {
        component_registry = &documentComponents();
    }
    BindingContext bindings;
    bindings.strings = strings;
    bindings.images = images;
    bindings.icons = icons;
    if (!fonts.empty())
    {
        bindings.owned_atlas = std::make_unique<detail::TextAtlasBuilder>(std::move(fonts));
        bindings.atlas = bindings.owned_atlas.get();
    }
    return instantiateConfigured(world, document, component_registry, bindings);
}

Entity instantiate(World& world, dom::Document& document,
                   const dom::ComponentRegistry* component_registry, TextRuntime& runtime,
                   const StringTable* strings, const ImageRegistry* images,
                   const IconRegistry* icons)
{
    if (component_registry == nullptr)
    {
        component_registry = &documentComponents();
    }
    BindingContext bindings;
    bindings.atlas = TextRuntimeAccess::atlas(runtime);
    bindings.runtime = &runtime;
    bindings.strings = strings;
    bindings.images = images;
    bindings.icons = icons;
    return instantiateConfigured(world, document, component_registry, bindings);
}

bool scrubValue(ecs::World& world, ecs::Entity entity, float& out_value)
{
    const Scrubber* scrubber = world.get<Scrubber>(entity);
    if (scrubber == nullptr)
    {
        return false;
    }
    out_value = scrubber->value;
    return true;
}

bool setScrubValue(ecs::World& world, ecs::Entity entity, float value)
{
    Scrubber* scrubber = world.get<Scrubber>(entity);
    if (scrubber == nullptr)
    {
        return false;
    }
    if (scrubber->value != value)
    {
        scrubber->value = value;
        world.markChanged<Scrubber>(entity);
    }
    return true;
}

ecs::Entity selectedChild(ecs::World& world, ecs::Entity container)
{
    if (!world.alive(container))
    {
        return ecs::Entity{};
    }
    for (const ecs::Entity child : hierarchy::children(world, container))
    {
        const Selectable* selectable = world.get<Selectable>(child);
        if (selectable != nullptr && selectable->selected)
        {
            return child;
        }
    }
    return ecs::Entity{};
}

std::string widgetText(ecs::World& world, ecs::Entity entity)
{
    if (!world.alive(entity))
    {
        return {};
    }
    std::vector<ecs::Entity> stack{entity};
    while (!stack.empty())
    {
        const ecs::Entity current = stack.front();
        stack.erase(stack.begin());
        if (const Text* text = world.get<Text>(current);
            text != nullptr && world.alive(text->content))
        {
            const auto* bytes = static_cast<const char*>(world.blobData(text->content));
            if (bytes != nullptr && world.blobSize(text->content) > 0U)
            {
                return std::string(bytes, world.blobSize(text->content));
            }
        }
        for (const ecs::Entity child : hierarchy::children(world, current))
        {
            stack.push_back(child);
        }
    }
    return {};
}

ecs::Entity findByKey(ecs::World& world, ecs::Entity canvas, std::string_view key)
{
    if (!world.alive(canvas))
    {
        return ecs::Entity{};
    }
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : key)
    {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ULL;
    }
    struct Search
    {
        std::uint64_t hash = 0U;
        ecs::Entity found{};
    };
    Search search{hash, ecs::Entity{}};
    const auto visit = [](Search& state, ecs::World& world, ecs::Entity entity)
    {
        const WidgetKey* key_component = world.get<WidgetKey>(entity);
        if (key_component != nullptr && key_component->hash == state.hash)
        {
            state.found = entity;
        }
    };
    std::vector<ecs::Entity> stack{canvas};
    while (!stack.empty())
    {
        const ecs::Entity entity = stack.back();
        stack.pop_back();
        visit(search, world, entity);
        if (search.found.valid())
        {
            return search.found;
        }
        for (const ecs::Entity child : hierarchy::children(world, entity))
        {
            stack.push_back(child);
        }
    }
    return ecs::Entity{};
}

} // namespace resonate::ui
