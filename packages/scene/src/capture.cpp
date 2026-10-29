#include <resonate/scene/capture.h>

#include <cmath>
#include <cstdio>
#include <string>

#include <resonate/hierarchy/name.h>
#include <resonate/hierarchy/tree.h>

#include "resonate.scene/components.gen.h"
#include "resonate.render/components.gen.h"
#include "resonate.hierarchy/components.gen.h"

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

/* Works for Vec4 and Quat: both carry x/y/z/w. */
template <typename T>
bool isDefault(const T& value, float x, float y, float z, float w)
{
    return std::fabs(value.x - x) < 1.0e-6F && std::fabs(value.y - y) < 1.0e-6F &&
           std::fabs(value.z - z) < 1.0e-6F && std::fabs(value.w - w) < 1.0e-6F;
}

bool isDefault(const resonate::Vec3& value, float x, float y, float z)
{
    return std::fabs(value.x - x) < 1.0e-6F && std::fabs(value.y - y) < 1.0e-6F &&
           std::fabs(value.z - z) < 1.0e-6F;
}

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

std::string formatVec3(const resonate::Vec3& value)
{
    return formatFloat(value.x) + " " + formatFloat(value.y) + " " + formatFloat(value.z);
}

std::string formatQuat(const resonate::Quat& value)
{
    return formatFloat(value.w) + " " + formatFloat(value.x) + " " + formatFloat(value.y) +
           " " + formatFloat(value.z);
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

/* One component element, or nothing when every field sits at its default. */
void appendTransform(std::string& out, const hierarchy::LocalTransform& transform)
{
    std::string attributes;
    appendAttribute(attributes, "position", formatVec3(transform.position));
    if (!isDefault(transform.rotation, 0.0F, 0.0F, 0.0F, 1.0F))
    {
        appendAttribute(attributes, "rotation", formatQuat(transform.rotation));
    }
    if (!isDefault(transform.scale, 1.0F, 1.0F, 1.0F))
    {
        appendAttribute(attributes, "scale", formatVec3(transform.scale));
    }
    out += "      <LocalTransform" + attributes + "/>\n";
}

void appendComponents(std::string& out, ecs::World& world, ecs::Entity entity)
{
    if (const auto* transform = world.get<hierarchy::LocalTransform>(entity);
        transform != nullptr)
    {
        appendTransform(out, *transform);
    }
    if (const auto* mesh = world.get<render::MeshInstance>(entity); mesh != nullptr)
    {
        std::string attributes;
        appendAttribute(attributes, "mesh", formatFloat(static_cast<float>(mesh->mesh)));
        if (!isDefault(mesh->color, 1.0F, 1.0F, 1.0F))
        {
            appendAttribute(attributes, "color", formatVec3(mesh->color));
        }
        out += "      <MeshInstance" + attributes + "/>\n";
    }
    if (const auto* material = world.get<render::Material>(entity); material != nullptr)
    {
        std::string attributes;
        if (material->texture != 0U)
        {
            appendAttribute(attributes, "texture",
                            formatFloat(static_cast<float>(material->texture)));
        }
        if (!isDefault(material->tint, 1.0F, 1.0F, 1.0F))
        {
            appendAttribute(attributes, "tint", formatVec3(material->tint));
        }
        if (std::fabs(material->roughness - 0.5F) > 1.0e-6F)
        {
            appendAttribute(attributes, "roughness", formatFloat(material->roughness));
        }
        if (std::fabs(material->metallic) > 1.0e-6F)
        {
            appendAttribute(attributes, "metallic", formatFloat(material->metallic));
        }
        if (!attributes.empty())
        {
            out += "      <Material" + attributes + "/>\n";
        }
    }
    if (const auto* camera = world.get<render::Camera>(entity); camera != nullptr)
    {
        std::string attributes;
        if (std::fabs(camera->fov_y_degrees - 60.0F) > 1.0e-6F)
        {
            appendAttribute(attributes, "fov_y_degrees", formatFloat(camera->fov_y_degrees));
        }
        if (std::fabs(camera->near_z - 0.1F) > 1.0e-6F)
        {
            appendAttribute(attributes, "near_z", formatFloat(camera->near_z));
        }
        if (std::fabs(camera->far_z - 1000.0F) > 1.0e-6F)
        {
            appendAttribute(attributes, "far_z", formatFloat(camera->far_z));
        }
        if (!attributes.empty())
        {
            out += "      <Camera" + attributes + "/>\n";
        }
    }
    if (const auto* light = world.get<render::DirectionalLight>(entity); light != nullptr)
    {
        std::string attributes;
        if (!isDefault(light->direction, 0.0F, -1.0F, 0.0F))
        {
            appendAttribute(attributes, "direction", formatVec3(light->direction));
        }
        if (!isDefault(light->color, 1.0F, 1.0F, 1.0F))
        {
            appendAttribute(attributes, "color", formatVec3(light->color));
        }
        if (std::fabs(light->intensity - 1.0F) > 1.0e-6F)
        {
            appendAttribute(attributes, "intensity", formatFloat(light->intensity));
        }
        out += "      <DirectionalLight" + attributes + "/>\n";
    }
    if (const auto* light = world.get<render::PointLight>(entity); light != nullptr)
    {
        std::string attributes;
        if (!isDefault(light->color, 1.0F, 1.0F, 1.0F))
        {
            appendAttribute(attributes, "color", formatVec3(light->color));
        }
        if (std::fabs(light->intensity - 1.0F) > 1.0e-6F)
        {
            appendAttribute(attributes, "intensity", formatFloat(light->intensity));
        }
        if (std::fabs(light->radius - 10.0F) > 1.0e-6F)
        {
            appendAttribute(attributes, "radius", formatFloat(light->radius));
        }
        out += "      <PointLight" + attributes + "/>\n";
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
    appendObject(out_xml, world, root, 0U);
    out_xml += "  </Scene>\n";
    out_xml += "</document>\n";
    return true;
}

} // namespace resonate::scene
