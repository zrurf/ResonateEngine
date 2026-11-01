#include <resonate/scene/document.h>

#include <string_view>
#include <vector>

#include <resonate/document_bind/bind.h>
#include <cstring>

#include <resonate/hierarchy/name.h>

#include "material.h"
#include <resonate/hierarchy/tree.h>

#include "resonate.scene/components.gen.h"

namespace resonate::scene
{
namespace
{

/* The stable id an Object's key becomes. The component lives in the UI module's
   schema, so it is reached by its qualified name and attached only when the
   host registered it: a scene without the UI module still loads, it just has no
   keys to resolve. */
constexpr std::string_view kWidgetKeyComponent = "resonate.ui.WidgetKey";

std::string_view nodeLabel(const dom::Document& document, dom::NodeHandle node)
{
    const dom::StringId key = document.key(node);
    if (key != dom::kInvalidString)
    {
        return document.text(key);
    }
    return "<object>";
}

bool attachKey(ecs::World& world, ecs::Entity entity, std::uint64_t hash, std::string& error)
{
    const ecs::ComponentIndex index =
        world.findComponent(std::string(kWidgetKeyComponent).c_str());
    if (index == ecs::kInvalidComponent)
    {
        return true;
    }
    if (world.add(entity, index, &hash) == nullptr)
    {
        error = "the node key could not be attached";
        return false;
    }
    return true;
}

ecs::Entity instantiateObject(ecs::World& world, const dom::Document& document,
                              dom::NodeHandle node, const dom::ComponentRegistry* components,
                              const ResourceAliases* aliases, ecs::CommandBuffer& commands,
                              const ecs::Entity parent, std::string& error)
{
    const ecs::Entity entity = world.create();
    if (!entity.valid())
    {
        error = "an entity could not be created";
        return ecs::Entity{};
    }

    const dom::StringId key = document.key(node);
    if (key != dom::kInvalidString)
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const char character : document.text(key))
        {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
            hash *= 1099511628211ULL;
        }
        if (!attachKey(world, entity, hash, error))
        {
            return ecs::Entity{};
        }
        /* The key's text rides along, so a capture writes the same key back. */
        const ecs::BlobHandle text =
            world.createBlob(entity, document.text(key).size(), document.text(key).data());
        if (!text.valid() || world.add(entity, scene::SourceKey{text}) == nullptr)
        {
            error = "the node key's text could not be attached";
            return ecs::Entity{};
        }
    }

    const dom::StringId name = document.name(node);
    if (name != dom::kInvalidString && !document.text(name).empty())
    {
        std::string name_error;
        (void)hierarchy::setName(world, entity, std::string(document.text(name)), name_error);
    }

    for (const dom::Component& component : document.components(node))
    {
        const std::string_view component_name = document.text(component.type);
        const dom::ComponentDesc* descriptor =
            components != nullptr ? components->find(component_name) : nullptr;
        if (descriptor == nullptr)
        {
            error = "node '" + std::string(nodeLabel(document, node)) + "': component '" +
                    std::string(component_name) + "' is not in the registry";
            return ecs::Entity{};
        }
        if (component_name == "Animation")
        {
            if (!applyAnimation(world, document, component, *descriptor, aliases, entity,
                                error))
            {
                error = "node '" + std::string(nodeLabel(document, node)) + "': " + error;
                return ecs::Entity{};
            }
            continue;
        }
        if (component_name == "Material")
        {
            if (descriptor == nullptr)
            {
                error = "node '" + std::string(nodeLabel(document, node)) + "': component '" +
                        std::string(component_name) + "' is not in the registry";
                return ecs::Entity{};
            }
            if (!applyMaterial(world, document, component, *descriptor, aliases, entity, error))
            {
                error = "node '" + std::string(nodeLabel(document, node)) + "': " + error;
                return ecs::Entity{};
            }
            continue;
        }
        if (!dom_bind::attachComponent(world, entity, document, component, *descriptor, error))
        {
            error = "node '" + std::string(nodeLabel(document, node)) + "': " + error;
            return ecs::Entity{};
        }
    }

    /* The propagation writes into an existing WorldTransform; every object
       carries the storage, as the C-composed scenes always did. */
    world.add(entity, hierarchy::WorldTransform{});

    if (parent.valid())
    {
        hierarchy::setParent(commands, entity, parent);
    }

    for (dom::NodeHandle child = document.firstChild(node); child.valid();
         child = document.nextSibling(child))
    {
        if (document.text(document.type(child)) != "Object")
        {
            error = "node '" + std::string(nodeLabel(document, node)) + "': a scene's children "
                                                                            "are Objects";
            return ecs::Entity{};
        }
        if (!instantiateObject(world, document, child, components, aliases, commands, entity,
                               error)
                 .valid())
        {
            return ecs::Entity{};
        }
    }
    return entity;
}

/* The Resources block a Scene may start with: every Resource key mapped to the
   asset its `asset` attribute names. A document without one answers an empty
   table. */
bool collectAliases(dom::Document& document, dom::NodeHandle scene, ResourceAliases& out,
                    std::string& error)
{
    const dom::StringId asset_property = document.intern("asset");
    for (dom::NodeHandle child = document.firstChild(scene); child.valid();
         child = document.nextSibling(child))
    {
        if (document.text(document.type(child)) != "Resources")
        {
            continue;
        }
        for (dom::NodeHandle entry = document.firstChild(child); entry.valid();
             entry = document.nextSibling(entry))
        {
            if (document.text(document.type(entry)) != "Resource")
            {
                continue;
            }
            const dom::StringId key = document.key(entry);
            const dom::Field* asset = document.property(entry, asset_property);
            if (key == dom::kInvalidString || asset == nullptr || asset->value.empty())
            {
                error = "a Resources entry needs a key and an asset";
                return false;
            }
            asset::AssetId id;
            std::string id_error;
            if (!parseAssetId(asset->value, id, id_error))
            {
                error = "resource '" + std::string(document.text(key)) + "': " + id_error;
                return false;
            }
            out[std::string(document.text(key))] = id;
        }
    }
    return true;
}

} // namespace

ecs::Entity findByKey(ecs::World& world, ecs::Entity root, std::string_view key)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : key)
    {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ULL;
    }
    const ecs::ComponentIndex index = world.findComponent(std::string(kWidgetKeyComponent).c_str());
    if (index == ecs::kInvalidComponent || !world.alive(root))
    {
        return ecs::Entity{};
    }
    std::vector<ecs::Entity> stack{root};
    while (!stack.empty())
    {
        const ecs::Entity entity = stack.back();
        stack.pop_back();
        if (const void* bytes = world.get(entity, index); bytes != nullptr)
        {
            std::uint64_t stored = 0U;
            std::memcpy(&stored, bytes, sizeof(stored));
            if (stored == hash)
            {
                return entity;
            }
        }
        for (const ecs::Entity child : hierarchy::children(world, entity))
        {
            stack.push_back(child);
        }
    }
    return ecs::Entity{};
}

bool instantiateUnder(ecs::World& world, dom::Document& document,
                      const dom::ComponentRegistry* components, ecs::Entity parent,
                      ecs::Entity& out_root, std::string& error,
                      std::vector<ecs::Entity>* out_roots)
{
    out_root = ecs::Entity{};
    /* The adapter's own components ride on keyed objects and aliased materials;
       registration is idempotent, and a world that already registered them (a
       second scene, a reload) reuses them. */
    if (world.registerComponent<SourceKey>() == ecs::kInvalidComponent ||
        world.registerComponent<TextureAlias>() == ecs::kInvalidComponent)
    {
        error = "the scene components could not be registered";
        return false;
    }
    const dom::NodeHandle root = document.root();
    if (!root.valid())
    {
        error = "the document has no root node";
        return false;
    }
    if (document.text(document.type(root)) != "Scene")
    {
        error = "the root node is not a Scene";
        return false;
    }

    ecs::CommandBuffer commands(world);
    ResourceAliases aliases;
    if (!collectAliases(document, root, aliases, error))
    {
        return false;
    }
    ecs::Entity first{};
    for (dom::NodeHandle child = document.firstChild(root); child.valid();
         child = document.nextSibling(child))
    {
        /* The alias table is not an object: it was read before the walk. */
        if (document.text(document.type(child)) == "Resources")
        {
            continue;
        }
        if (document.text(document.type(child)) != "Object")
        {
            error = "node '" + std::string(nodeLabel(document, child)) +
                    "': a Scene holds Objects and one Resources block";
            return false;
        }
        const ecs::Entity entity =
            instantiateObject(world, document, child, components, &aliases, commands, parent,
                              error);
        if (!entity.valid())
        {
            return false;
        }
        if (!first.valid())
        {
            first = entity;
        }
        if (out_roots != nullptr)
        {
            out_roots->push_back(entity);
        }
    }
    world.play(commands);
    out_root = first;
    return true;
}

bool instantiate(ecs::World& world, dom::Document& document,
                 const dom::ComponentRegistry* components, ecs::Entity& out_root,
                 std::string& error, std::vector<ecs::Entity>* out_roots)
{
    return instantiateUnder(world, document, components, ecs::Entity{}, out_root, error,
                            out_roots);
}

} // namespace resonate::scene
