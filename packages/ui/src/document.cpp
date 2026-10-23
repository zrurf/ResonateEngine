#include <resonate/ui/document.h>

#include <resonate/ui/document_schema.h>
#include <resonate/ui/style.h>

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
        else
        {
            error = "unknown LayoutGroup field '" + std::string(name) + "'";
            return false;
        }
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

struct BindingContext
{
    Entity canvas{};
    ecs::ComponentIndex data_component = ecs::kInvalidComponent;
    std::vector<detail::BindingRecord> records;
    dom::StringId text_property = dom::kInvalidString;
    std::unique_ptr<detail::TextAtlasBuilder> atlas;
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
            if (!property->value.empty())
            {
                const ecs::BlobHandle content =
                    world.createBlob(entity, property->value.size(), property->value.data());
                if (!content.valid())
                {
                    return report(world, document, node, "the text bytes could not be attached");
                }
                text->content = content;
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
    const float width = rect != nullptr ? rect->width : 0.0F;
    std::vector<detail::GlyphQuad> glyphs;
    std::string error;
    if (!context.atlas->buildRun(std::string_view(bytes, size), text->font_size, width,
                                 static_cast<std::uint32_t>(text->align), text->wrap, glyphs,
                                 error))
    {
        return report(world, document, node, "text: " + error);
    }
    if (glyphs.empty())
    {
        return true;
    }
    const ecs::BlobHandle records =
        world.createBlob(entity, glyphs.size() * sizeof(detail::GlyphQuad), glyphs.data());
    if (!records.valid() || world.add(entity, Glyphs{records}) == nullptr)
    {
        return report(world, document, node, "glyph quads could not be attached");
    }
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
    for (std::uint32_t state = 1U; state <= 3U; ++state)
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
                std::string error;
                if (!detail::parseField(*layout, normal_field->value, value.value, error))
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
                if (!detail::parseField(*layout, styled_field.value, value.value, error))
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

} // namespace

Entity instantiate(World& world, dom::Document& document,
                   const dom::ComponentRegistry* component_registry,
                   std::shared_ptr<const text::Font> font)
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
    BindingContext bindings;
    bindings.text_property = document.intern("text");
    if (font != nullptr)
    {
        bindings.atlas = std::make_unique<detail::TextAtlasBuilder>(std::move(font));
    }
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
    if (world.add(entity, PointerState{}) == nullptr)
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
            world.add(entity, TextAtlasPage{bindings.atlas->width(), bindings.atlas->height(), 1U,
                                            page}) == nullptr)
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

} // namespace resonate::ui
