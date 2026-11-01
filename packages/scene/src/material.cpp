#include "material.h"

#include <cstdlib>
#include <map>
#include <string>
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
    /* The node composition hangs a fieldless Animation on every object; a
       missing clip is that default, and the null id means no reference. */
    (void)has_clip;
    authored.clip_low = clip.guid_low;
    authored.clip_high = clip.guid_high;
    if (world.add(entity, authored) == nullptr)
    {
        error = "Animation could not be attached";
        return false;
    }
    return true;
}

/* Escapes one attribute value for the stored fragment. */
std::string escapeAttribute(std::string_view text)
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

bool applyMachine(ecs::World& world, dom::Document& document, dom::NodeHandle node,
                  const ResourceAliases* aliases, ecs::Entity entity,
                  animation::AnimationPlayer* player, std::string& error)
{
    if (player == nullptr)
    {
        /* A host without an animation player loads the scene without the
           machine: the objects stand still rather than the load failing. */
        world.report(ecs::World::Report::Warning,
                     "animation: the scene has a machine but the run has no player");
        return true;
    }
    animation::StateMachine machine;
    std::map<std::string, std::uint16_t> names;
    std::vector<std::pair<std::string, asset::AssetId>> machine_aliases;
    std::string entry_name;
    /* The authored element, rebuilt from what the document actually said: the
       built definition lives in the player, so this text is what a capture
       writes back. */
    std::string fragment = "<Machine";
    if (const dom::Field* entry = document.property(node, document.intern("entry"));
        entry != nullptr && !entry->value.empty())
    {
        entry_name.assign(entry->value);
        fragment += " entry=\"" + escapeAttribute(entry->value) + "\"";
    }
    fragment += ">\n";
    for (dom::NodeHandle child = document.firstChild(node); child.valid();
         child = document.nextSibling(child))
    {
        const std::string_view type = document.text(document.type(child));
        if (type != "State")
        {
            continue;
        }
        const dom::StringId name_id = document.name(child);
        const dom::Field* clip = document.property(child, document.intern("clip"));
        if (name_id == dom::kInvalidString || clip == nullptr || clip->value.empty())
        {
            error = "a Machine state needs a name and a clip";
            return false;
        }
        const std::string name(document.text(name_id));
        if (names.find(name) != names.end())
        {
            error = "the Machine names state '" + name + "' twice";
            return false;
        }
        std::string looping_text;
        if (const dom::Field* authored = document.property(child, document.intern("looping"));
            authored != nullptr)
        {
            if (authored->value != "true" && authored->value != "false")
            {
                error = "the state '" + name + "' has looping='" + std::string(authored->value) +
                        "'; it is true or false";
                return false;
            }
            looping_text.assign(authored->value);
        }
        asset::AssetId id{};
        const std::string_view reference = clip->value;
        if (!reference.empty() && reference.front() == '#')
        {
            const std::string alias_name(reference.substr(1U));
            const auto found =
                aliases != nullptr ? aliases->find(alias_name) : ResourceAliases::iterator{};
            if (aliases == nullptr || found == aliases->end())
            {
                error = "the state '" + name + "' has no resource '" + alias_name + "'";
                return false;
            }
            id = found->second;
            machine_aliases.emplace_back(alias_name, id);
        }
        else
        {
            std::string id_error;
            if (!parseAssetId(reference, id, id_error))
            {
                error = "the state '" + name + "': " + id_error;
                return false;
            }
        }
        std::uint32_t clip_index = 0U;
        if (!player->clipFor(id, clip_index, error))
        {
            error = "the state '" + name + "': " + error;
            return false;
        }
        const animation::Clip* played = player->clip(clip_index);
        machine.states.push_back(
            animation::State{clip_index, played != nullptr ? played->duration : 0.0F,
                             looping_text.empty() || looping_text == "true"});
        names.emplace(name, static_cast<std::uint16_t>(machine.states.size() - 1U));
        fragment += "  <State name=\"" + escapeAttribute(name) + "\" clip=\"" +
                    escapeAttribute(clip->value) + "\"";
        if (!looping_text.empty())
        {
            fragment += " looping=\"" + looping_text + "\"";
        }
        fragment += "/>\n";
    }
    if (machine.states.empty())
    {
        error = "the Machine has no states";
        return false;
    }
    std::fprintf(stderr, "DBG machine states=%zu\n", machine.states.size());
    const auto entry_found = names.find(entry_name);
    if (!entry_name.empty() && entry_found == names.end())
    {
        error = "the Machine's entry '" + entry_name + "' names no state";
        return false;
    }
    machine.initial = entry_found != names.end() ? entry_found->second : 0U;

    const auto stateIndex = [&names](const std::string& name, std::uint16_t& out)
    {
        if (name == "*")
        {
            out = animation::kAnyState;
            return true;
        }
        const auto found = names.find(name);
        if (found == names.end())
        {
            return false;
        }
        out = found->second;
        return true;
    };
    for (dom::NodeHandle child = document.firstChild(node); child.valid();
         child = document.nextSibling(child))
    {
        if (document.text(document.type(child)) != "Transition")
        {
            continue;
        }
        animation::Transition edge;
        std::string from_text;
        std::string to_text;
        const dom::Field* from = document.property(child, document.intern("from"));
        const dom::Field* to = document.property(child, document.intern("to"));
        if (from == nullptr || to == nullptr)
        {
            error = "a Machine transition needs from and to";
            return false;
        }
        from_text.assign(from->value);
        to_text.assign(to->value);
        if (!stateIndex(from_text, edge.from) || !stateIndex(to_text, edge.to))
        {
            error = "a Machine transition names an unknown state";
            return false;
        }
        fragment += "  <Transition from=\"" + escapeAttribute(from_text) + "\" to=\"" +
                    escapeAttribute(to_text) + "\"";
        if (const dom::Field* authored = document.property(child, document.intern("at_end"));
            authored != nullptr)
        {
            if (authored->value != "true" && authored->value != "false")
            {
                error = "at_end is true or false";
                return false;
            }
            edge.at_end_only = authored->value == "true";
            fragment += " at_end=\"" + authored->value + "\"";
        }
        if (const dom::Field* authored = document.property(child, document.intern("fade"));
            authored != nullptr)
        {
            edge.fade_seconds = std::strtof(std::string(authored->value).c_str(), nullptr);
            fragment += " fade=\"" + escapeAttribute(authored->value) + "\"";
        }
        if (const dom::Field* authored = document.property(child, document.intern("sync"));
            authored != nullptr)
        {
            if (authored->value != "true" && authored->value != "false")
            {
                error = "sync is true or false";
                return false;
            }
            edge.sync = authored->value == "true";
            fragment += " sync=\"" + authored->value + "\"";
        }
        fragment += "/>\n";
        machine.transitions.push_back(edge);
    }
    if (!machine.validate(error))
    {
        return false;
    }
    fragment += "</Machine>";
    /* The alias references travel with the machine: a capture rebuilds the
       Resources block from them, since nothing else remembers a machine
       state's clip alias. */
    for (const auto& [alias_name, id] : machine_aliases)
    {
        std::string asset_text = "@" + asset::formatGuid(id);
        if (id.sub_id != 1U)
        {
            asset_text += ":" + std::to_string(id.sub_id);
        }
        fragment = "    <Resource key=\"" + escapeAttribute(alias_name) + "\" asset=\"" +
                   asset_text + "\"/>\n" + fragment;
    }

    const std::uint16_t initial = machine.initial;
    const animation::State& entry_state = machine.states[initial];
    const std::uint32_t machine_index = player->addMachine(std::move(machine));
    animation::AnimationState state;
    state.clip = entry_state.clip;
    state.looping = entry_state.looping;
    state.driven = true;
    const ecs::BlobHandle text = world.createBlob(entity, fragment.size(), fragment.data());
    if (world.add(entity, state) == nullptr ||
        text == ecs::BlobHandle{} ||
        world.add(entity, scene::MachineSource{text}) == nullptr ||
        world.add(entity, animation::MachineState{machine_index, animation::MachineRuntime{}}) ==
            nullptr)
    {
        error = "the machine could not be attached";
        return false;
    }
    return true;
}

} // namespace resonate::scene
