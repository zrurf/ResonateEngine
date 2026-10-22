#include <resonate/ui/document.h>

#include <resonate/ui/style.h>

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

bool applyStyleRef(const dom::Document& document, const dom::Component& component,
                   StyleRef& style, std::string& error)
{
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

bool applyComponents(World& world, const dom::Document& document, dom::NodeHandle node,
                     Entity entity, const std::vector<dom::Component>& components)
{
    const dom::Component* computed = nullptr;
    for (const dom::Component& component : components)
    {
        const std::string_view name = baseName(document.text(component.type));
        if (name == "ComputedRect")
        {
            computed = &component;
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
        else if (name == "StyleRef")
        {
            StyleRef value;
            applied = applyStyleRef(document, component, value, error) &&
                      world.add(entity, value) != nullptr;
        }
        else
        {
            error = "unknown component '" + std::string(document.text(component.type)) + "'";
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
    if (world.add(entity, ComputedRect{}) == nullptr)
    {
        return report(world, document, node, "ComputedRect could not be attached");
    }
    return true;
}

Entity instantiateNode(World& world, CommandBuffer& commands, const dom::Document& document,
                       dom::NodeHandle node, const StyleTable& styles,
                       std::vector<Entity>& created)
{
    const Entity entity = world.create();
    if (!entity.valid())
    {
        report(world, document, node, "an entity could not be created");
        return Entity{};
    }
    created.push_back(entity);

    if (!applyComponents(world, document, node, entity, styles.resolve(document, node)))
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
            instantiateNode(world, commands, document, child, styles, created);
        if (!child_entity.valid())
        {
            return Entity{};
        }
        resonate::hierarchy::setParent(commands, child_entity, entity);
    }
    return entity;
}

} // namespace

Entity instantiate(World& world, dom::Document& document)
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
    const Entity entity = instantiateNode(world, commands, document, root, styles, created);
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
    world.play(commands);
    return entity;
}

} // namespace resonate::ui
