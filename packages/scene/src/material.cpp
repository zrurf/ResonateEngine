#include "material.h"

#include <string_view>

#include <resonate/asset/identity.h>
#include <resonate/document_bind/bind.h>

#include "resonate.render/components.gen.h"
#include "resonate.scene/components.gen.h"
#include "resonate.animation/components.gen.h"

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

bool parseAssetId(std::string_view text, asset::AssetId& out, std::string& error)
{
    if (!text.empty() && text.front() == '@')
    {
        text.remove_prefix(1U);
    }
    std::string_view guid = text;
    std::uint32_t sub_id = 1U;
    const std::size_t colon = text.find(':');
    if (colon != std::string_view::npos)
    {
        guid = text.substr(0U, colon);
        if (!parseU32(text.substr(colon + 1U), sub_id))
        {
            error = "the sub id is not a number";
            return false;
        }
    }
    asset::AssetId parsed;
    if (!asset::parseGuid(guid, parsed))
    {
        error = "the value is not an asset id";
        return false;
    }
    parsed.sub_id = sub_id;
    out = parsed;
    return true;
}

bool applyMaterial(ecs::World& world, const dom::Document& document,
                   const dom::Component& component, const dom::ComponentDesc& descriptor,
                   const ResourceAliases* aliases, ecs::Entity entity, std::string& error)
{
    /* The generated struct carries the schema defaults; authored fields
       overwrite them. */
    render::Material material;
    render::TextureRef reference;
    bool has_reference = false;
    std::string alias_text;
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "texture" && !field.value.empty() &&
            (field.value.front() == '@' || field.value.front() == '#'))
        {
            asset::AssetId id;
            if (field.value.front() == '#')
            {
                alias_text = std::string(field.value.substr(1U));
                if (aliases == nullptr)
                {
                    error = "Material.texture: '" + field.value +
                            "' needs a Resources block";
                    return false;
                }
                const auto found = aliases->find(alias_text);
                if (found == aliases->end())
                {
                    error = "Material.texture: no resource '" + alias_text +
                            "' in the Resources block";
                    return false;
                }
                id = found->second;
            }
            else
            {
                std::string id_error;
                if (!parseAssetId(field.value, id, id_error))
                {
                    error = "Material.texture: " + id_error;
                    return false;
                }
            }
            reference.guid_low = id.guid_low;
            reference.guid_high = id.guid_high;
            reference.sub_id = id.sub_id;
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
    if (!alias_text.empty())
    {
        const ecs::BlobHandle text =
            world.createBlob(entity, alias_text.size(), alias_text.data());
        if (!text.valid() || world.add(entity, TextureAlias{text}) == nullptr)
        {
            error = "TextureAlias could not be attached";
            return false;
        }
    }
    return true;
}

bool applyAnimation(ecs::World& world, const dom::Document& document,
                    const dom::Component& component, const dom::ComponentDesc& descriptor,
                    const ResourceAliases* aliases, ecs::Entity entity, std::string& error)
{
    animation::Animation authored;
    bool has_clip = false;
    asset::AssetId clip{};
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        if (name == "clip" && !field.value.empty() &&
            (field.value.front() == '@' || field.value.front() == '#'))
        {
            if (field.value.front() == '#')
            {
                const std::string alias_name(field.value.substr(1U));
                if (aliases == nullptr)
                {
                    error = "Animation.clip: '" + field.value + "' needs a Resources block";
                    return false;
                }
                const auto found = aliases->find(alias_name);
                if (found == aliases->end())
                {
                    error = "Animation.clip: no resource '" + alias_name +
                            "' in the Resources block";
                    return false;
                }
                clip = found->second;
            }
            else
            {
                std::string id_error;
                if (!parseAssetId(field.value, clip, id_error))
                {
                    error = "Animation.clip: " + id_error;
                    return false;
                }
            }
            has_clip = true;
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
            error = "unknown field '" + std::string(name) + "' on Animation";
            return false;
        }
        if (!dom_bind::parseField(*layout, field.value, &authored, error))
        {
            error = std::string("Animation.") + std::string(name) + ": " + error;
            return false;
        }
    }
    if (!has_clip)
    {
        error = "Animation needs a clip";
        return false;
    }
    authored.clip_low = clip.guid_low;
    authored.clip_high = clip.guid_high;
    if (world.add(entity, authored) == nullptr)
    {
        error = "Animation could not be attached";
        return false;
    }
    return true;
}

} // namespace resonate::scene
