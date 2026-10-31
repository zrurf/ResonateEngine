#include <resonate/scene/capture.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <resonate/asset/identity.h>
#include <resonate/hierarchy/name.h>
#include <resonate/hierarchy/tree.h>

#include "resonate.scene/components.gen.h"
#include "resonate.scene/nodes.gen.h"
#include "resonate.render/components.gen.h"
#include "resonate.render/nodes.gen.h"
#include "resonate.hierarchy/components.gen.h"
#include "resonate.hierarchy/nodes.gen.h"

namespace resonate::scene
{
namespace
{

/* The scene schema's field defaults, mirrored from the .rschema: a field whose
   bytes match its default is not written, so captures stay readable. Changing
   a default in the schema must change it here. */
struct Vec3Default
{
    float x, y, z;
};

/* Shortest text that reads back as the same float: "%g" at widening precision
   until an strtod round trip lands on the bit-identical value. */
std::string formatFloat(float value)
{
    char text[32] = {};
    for (int precision = 6; precision <= 9; ++precision)
    {
        std::snprintf(text, sizeof(text), "%.*g", precision,
                      static_cast<double>(value));
        if (std::strtof(text, nullptr) == value)
        {
            break;
        }
    }
    return text;
}

/* The field kinds the writer formats, by the descriptor's type name and size.
   The binder reads a vec or a quat as its members in declaration order, so the
   writer emits them the same way. */
enum class FieldKind
{
    Float,
    Int32,
    Uint32,
    Uint64,
    Bool,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Unsupported,
};

FieldKind kindOf(const dom::ComponentFieldDesc& field)
{
    const std::string_view type = field.type;
    if (type == "f32" && field.size == sizeof(float))
    {
        return FieldKind::Float;
    }
    if (type == "i32" && field.size == sizeof(std::int32_t))
    {
        return FieldKind::Int32;
    }
    if (type == "u32" && field.size == sizeof(std::uint32_t))
    {
        return FieldKind::Uint32;
    }
    if (type == "u64" && field.size == sizeof(std::uint64_t))
    {
        return FieldKind::Uint64;
    }
    if (type == "bool" && field.size == 1U)
    {
        return FieldKind::Bool;
    }
    if (type == "vec2" && field.size == sizeof(Vec2))
    {
        return FieldKind::Vec2;
    }
    if (type == "vec3" && field.size == sizeof(Vec3))
    {
        return FieldKind::Vec3;
    }
    if (type == "vec4" && field.size == sizeof(Vec4))
    {
        return FieldKind::Vec4;
    }
    if (type == "quat" && field.size == sizeof(Quat))
    {
        return FieldKind::Quat;
    }
    return FieldKind::Unsupported;
}

/* One field's authoring text, or false for a kind the writer cannot format. */
bool formatField(std::string& out, const dom::ComponentFieldDesc& field,
                 const std::byte* value)
{
    const auto floats = [&out](const std::byte* bytes, std::size_t count)
    {
        const float* values = reinterpret_cast<const float*>(bytes);
        for (std::size_t index = 0U; index < count; ++index)
        {
            if (index > 0U)
            {
                out += " ";
            }
            out += formatFloat(values[index]);
        }
    };
    switch (kindOf(field))
    {
    case FieldKind::Float:
    {
        float scalar = 0.0F;
        std::memcpy(&scalar, value, sizeof(scalar));
        out += formatFloat(scalar);
        return true;
    }
    case FieldKind::Int32:
    {
        std::int32_t scalar = 0;
        std::memcpy(&scalar, value, sizeof(scalar));
        out += std::to_string(scalar);
        return true;
    }
    case FieldKind::Uint32:
    {
        std::uint32_t scalar = 0U;
        std::memcpy(&scalar, value, sizeof(scalar));
        out += std::to_string(scalar);
        return true;
    }
    case FieldKind::Uint64:
    {
        std::uint64_t scalar = 0U;
        std::memcpy(&scalar, value, sizeof(scalar));
        out += std::to_string(scalar);
        return true;
    }
    case FieldKind::Bool:
        out += *value == std::byte{0} ? "false" : "true";
        return true;
    case FieldKind::Vec2:
        floats(value, 2U);
        return true;
    case FieldKind::Vec3:
        floats(value, 3U);
        return true;
    case FieldKind::Vec4:
        floats(value, 4U);
        return true;
    case FieldKind::Quat:
        floats(value, 4U);
        return true;
    case FieldKind::Unsupported:
        break;
    }
    return false;
}

/* Components the adapter owns: parenting is the document's nesting, the name
   is the Object's core attribute, and the world transform is the hierarchy
   pass's to write. Authoring any of them would fight the adapter. */
bool adapterManaged(std::string_view qualified_name)
{
    for (const std::string_view name :
         {"resonate.hierarchy.Name", "resonate.hierarchy.Parent",
          "resonate.hierarchy.Children", "resonate.hierarchy.WorldTransform"})
    {
        if (name == qualified_name)
        {
            return true;
        }
    }
    return false;
}

void appendAttribute(std::string& out, const char* name, const std::string& value)
{
    out += " ";
    out += name;
    out += "=\"";
    out += value;
    out += "\"";
}

std::string escaped(std::string_view text)
{
    std::string out;
    for (const char character : text)
    {
        switch (character)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(character);
        }
    }
    return out;
}

std::string_view blobText(ecs::World& world, const ecs::BlobHandle& handle)
{
    if (!handle.valid() || !world.alive(handle))
    {
        return {};
    }
    const auto* bytes = static_cast<const char*>(world.blobData(handle));
    const std::size_t size = world.blobSize(handle);
    if (bytes == nullptr || size == 0U)
    {
        return {};
    }
    return std::string_view(bytes, size);
}

std::string_view keyText(ecs::World& world, ecs::Entity entity)
{
    const SourceKey* key = world.get<SourceKey>(entity);
    return key != nullptr ? blobText(world, key->text) : std::string_view{};
}

std::string_view nameText(ecs::World& world, ecs::Entity entity)
{
    const hierarchy::Name* name = world.get<hierarchy::Name>(entity);
    return name != nullptr ? blobText(world, name->text) : std::string_view{};
}

/* One component element for an object that carries it: fields equal to the
   schema default are left out, the element itself is always written so the
   component survives the round trip. Material's texture is authored from the
   resolved reference or the alias, never from the slot, so that field is
   skipped and the attribute supplied here. */
void appendComponents(std::string& out, ecs::World& world, ecs::Entity entity)
{
    struct Table
    {
        const dom::ComponentDesc* descs;
        std::size_t count;
    };
    const Table tables[] = {
        {hierarchy::generated::kComponentDescs, hierarchy::generated::kComponentDescCount},
        {render::generated::kComponentDescs, render::generated::kComponentDescCount},
        {scene::generated::kComponentDescs, scene::generated::kComponentDescCount}};
    for (const Table& table : tables)
    {
        for (std::size_t index = 0U; index < table.count; ++index)
        {
            const dom::ComponentDesc& descriptor = table.descs[index];
            if (!capturable(descriptor))
            {
                continue;
            }
            const ecs::ComponentIndex component =
                world.findComponent(descriptor.qualified_name);
            if (component == ecs::kInvalidComponent)
            {
                continue;
            }
            const void* bytes = world.get(entity, component);
            if (bytes == nullptr)
            {
                continue;
            }
            const auto* base = static_cast<const std::byte*>(bytes);
            const auto* defaults = static_cast<const std::byte*>(descriptor.defaults);
            const bool material = std::string_view(descriptor.name) == "Material";
            std::string attributes;
            if (material)
            {
                /* A resolved reference is written back as the authoring form,
                   not as the slot it resolved to: the slot depends on
                   allocation order. An alias is shorter and is what the
                   document said. */
                std::string texture;
                if (const auto* alias = world.get<TextureAlias>(entity);
                    alias != nullptr && !blobText(world, alias->text).empty())
                {
                    texture = "#" + std::string(blobText(world, alias->text));
                }
                else if (const auto* reference = world.get<render::TextureRef>(entity);
                         reference != nullptr && reference->sub_id != 0U)
                {
                    const asset::AssetId id{reference->guid_low, reference->guid_high,
                                            reference->sub_id};
                    texture = "@" + asset::formatGuid(id);
                    if (reference->sub_id != 1U)
                    {
                        texture += ":" + std::to_string(reference->sub_id);
                    }
                }
                else if (const auto* saved = world.get<render::Material>(entity);
                         saved != nullptr && saved->texture != 0U)
                {
                    texture = formatFloat(static_cast<float>(saved->texture));
                }
                if (!texture.empty())
                {
                    appendAttribute(attributes, "texture", texture);
                }
            }
            for (const dom::ComponentFieldDesc& field : descriptor.fields)
            {
                if (material && std::string_view(field.name) == "texture")
                {
                    continue;
                }
                const std::byte* value = base + field.offset;
                if (defaults != nullptr &&
                    std::memcmp(value, defaults + field.offset, field.size) == 0)
                {
                    continue;
                }
                std::string text;
                if (formatField(text, field, value))
                {
                    appendAttribute(attributes, field.name, text);
                }
                else
                {
                    world.report(ecs::World::Report::Warning,
                                 "scene capture: %s.%s has a kind it cannot write",
                                 descriptor.name, field.name);
                }
            }
            out += "      <";
            out += descriptor.name;
            out += attributes;
            out += "/>\n";
        }
    }
}

/* The aliases a subtree references: a material authored through "#name" names
   the asset its TextureRef resolved to, so the capture can write one Resource
   per name it uses. Sorted by name for a stable document. */
void collectAliases(ecs::World& world, ecs::Entity entity,
                    std::map<std::string, asset::AssetId>& out)
{
    if (const auto* alias = world.get<TextureAlias>(entity); alias != nullptr)
    {
        const std::string_view name = blobText(world, alias->text);
        const auto* reference = world.get<render::TextureRef>(entity);
        if (!name.empty() && reference != nullptr)
        {
            out[std::string(name)] = asset::AssetId{reference->guid_low, reference->guid_high,
                                                    reference->sub_id};
        }
    }
    for (const ecs::Entity child : hierarchy::children(world, entity))
    {
        collectAliases(world, child, out);
    }
}

void appendObject(std::string& out, ecs::World& world, ecs::Entity entity, std::size_t depth)
{
    const std::string indent(static_cast<std::size_t>(4 + depth * 2U), ' ');
    out += indent;
    out += "<Object";
    const std::string_view key = keyText(world, entity);
    if (!key.empty())
    {
        appendAttribute(out, "key", escaped(key));
    }
    const std::string_view name = nameText(world, entity);
    if (!name.empty())
    {
        appendAttribute(out, "name", escaped(name));
    }
    out += ">\n";
    appendComponents(out, world, entity);
    for (const ecs::Entity child : hierarchy::children(world, entity))
    {
        appendObject(out, world, child, depth + 1U);
    }
    out += indent;
    out += "</Object>\n";
}

} // namespace

bool capturable(const dom::ComponentDesc& component)
{
    if (component.derived != 0U || adapterManaged(component.qualified_name))
    {
        return false;
    }
    for (const dom::ComponentFieldDesc& field : component.fields)
    {
        if (kindOf(field) == FieldKind::Unsupported)
        {
            return false;
        }
    }
    return true;
}

ecs::Entity rootOf(ecs::World& world, ecs::Entity entity)
{
    ecs::Entity top = entity;
    for (int level = 0; level < hierarchy::kMaxDepth; ++level)
    {
        const ecs::Entity parent = hierarchy::parentOf(world, top);
        if (!parent.valid() || !world.alive(parent))
        {
            break;
        }
        top = parent;
    }
    return top;
}

bool capture(ecs::World& world, ecs::Entity root, std::string& out_xml, std::string& error)
{
    if (!world.alive(root))
    {
        error = "the scene root is not alive";
        return false;
    }
    out_xml.clear();
    out_xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    out_xml += "<document version=\"1\" schemas=\"resonate.scene\">\n";
    out_xml += "  <Scene>\n";
    std::map<std::string, asset::AssetId> aliases;
    collectAliases(world, root, aliases);
    if (!aliases.empty())
    {
        out_xml += "    <Resources>\n";
        for (const auto& [name, id] : aliases)
        {
            std::string asset_text = "@" + asset::formatGuid(id);
            if (id.sub_id != 1U)
            {
                asset_text += ":" + std::to_string(id.sub_id);
            }
            out_xml += "      <Resource key=\"" + escaped(name) + "\" asset=\"" + asset_text +
                       "\"/>\n";
        }
        out_xml += "    </Resources>\n";
    }
    appendObject(out_xml, world, root, 0U);
    out_xml += "  </Scene>\n";
    out_xml += "</document>\n";
    return true;
}

} // namespace resonate::scene
