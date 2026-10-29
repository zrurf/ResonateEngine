#include "material.h"

#include <string_view>

#include <resonate/asset/identity.h>
#include <resonate/document_bind/bind.h>

#include "resonate.render/components.gen.h"

namespace resonate::scene
{
namespace
{

bool parseU32(std::string_view text, std::uint32_t& out)
{
    if (text.empty())
    {
        return false;
    }
    std::uint32_t value = 0U;
    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        value = value * 10U + static_cast<std::uint32_t>(character - '0');
    }
    out = value;
    return true;
}

} // namespace

bool applyMaterial(ecs::World& world, const dom::Document& document,
                   const dom::Component& component, const dom::ComponentDesc& descriptor,
                   ecs::Entity entity, std::string& error)
{
    /* The generated struct carries the schema defaults; authored fields
       overwrite them. */
    render::Material material;
    render::TextureRef reference;
    bool has_reference = false;
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "texture" && !field.value.empty() && field.value.front() == '@')
        {
            /* "@<guid>[:<sub>]": an asset id the renderer resolves through the
               products manifest its textures were cooked with. */
            std::string_view id = field.value; /* the view borrows the owned field text */
            id.remove_prefix(1U);
            const std::size_t colon = id.find(':');
            std::string_view guid = id;
            if (colon != std::string_view::npos)
            {
                id = id.substr(colon + 1U);
                guid = guid.substr(0U, colon);
            }
            std::uint32_t sub_id = 1U;
            if (colon != std::string_view::npos && !parseU32(id, sub_id))
            {
                error = "Material.texture: the sub id is not a number";
                return false;
            }
            resonate::asset::AssetId asset;
            if (!resonate::asset::parseGuid(guid, asset))
            {
                error = "Material.texture: the value is not an asset id or a slot";
                return false;
            }
            reference.guid_low = asset.guid_low;
            reference.guid_high = asset.guid_high;
            reference.sub_id = sub_id;
            has_reference = true;
            continue;
        }
        const dom::ComponentFieldDesc* layout = nullptr;
        for (const dom::ComponentFieldDesc& candidate : descriptor.fields)
        {
            if (name == candidate.name)
            {
                layout = &candidate;
                break;
            }
        }
        if (layout == nullptr)
        {
            error = "unknown field '" + std::string(name) + "' on Material";
            return false;
        }
        if (!dom_bind::parseField(*layout, field.value, &material, error))
        {
            error = std::string("Material.") + std::string(name) + ": " + error;
            return false;
        }
    }
    if (world.add(entity, material) == nullptr)
    {
        error = "Material could not be attached";
        return false;
    }
    if (has_reference && world.add(entity, reference) == nullptr)
    {
        error = "TextureRef could not be attached";
        return false;
    }
    return true;
}

} // namespace resonate::scene
