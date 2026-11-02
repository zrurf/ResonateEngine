#include <resonate/startup/startup.hpp>

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <resonate/animation/player.h>
#include <resonate/application/save.h>
#include <resonate/application/snapshot.h>
#include <resonate/hierarchy/tree.h>

#include "resonate.gameplay/nodes.gen.h"

namespace resonate::startup
{
namespace
{

/* The save domain's version: the payload shape a slot carries. The snapshot's
   own format version is separate; component layouts migrate through the schema
   registry below. A version bump needs a migration for the slots a release
   already wrote. */
constexpr std::uint32_t kSceneSaveVersion = 1U;
constexpr const char* kSaveApplication = "ResonateEngine";

const save::MigrationRegistry& contentMigrations()
{
    static const save::MigrationRegistry registry = []
    {
        save::MigrationRegistry migrations;
        /* One entry per schema-declared component that has changed layout since
           a release wrote saves. The callback writes the current layout. */
        (void)migrations.add("resonate.gameplay.Health", 1U,
                             &gameplay::generated::kHealthMigrateFrom1);
        return migrations;
    }();
    return registry;
}

/* The components the player owns and the snapshot copies as bytes: their
   indices refer to this process's clip and machine tables, so a load is valid
   for a run composed from the same content. A layout change to either struct
   is a reason to bump the save domain version, not to migrate them. */
std::vector<ecs::ComponentIndex> opaqueComponents(const ecs::World& world)
{
    std::vector<ecs::ComponentIndex> components;
    for (const char* const name :
         {"resonate.animation.AnimationState", "resonate.animation.MachineState"})
    {
        const ecs::ComponentIndex index = world.findComponent(name);
        if (index != ecs::kInvalidComponent)
        {
            components.push_back(index);
        }
    }
    return components;
}

/* The scene's entities: the roots and everything below them. A host entity the
   caller did not name (the UI tree) is not part of the save. */
std::vector<ecs::Entity> sceneEntities(const ecs::World& world,
                                       const std::vector<ecs::Entity>& roots)
{
    std::vector<ecs::Entity> pending(roots.begin(), roots.end());
    std::vector<ecs::Entity> entities;
    std::unordered_set<std::uint64_t> seen;
    while (!pending.empty())
    {
        const ecs::Entity entity = pending.back();
        pending.pop_back();
        if (!world.alive(entity))
        {
            continue;
        }
        const std::uint64_t key = (static_cast<std::uint64_t>(entity.index) << 32U) |
                                  entity.generation;
        if (!seen.insert(key).second)
        {
            continue;
        }
        entities.push_back(entity);
        for (const ecs::Entity child : hierarchy::children(world, entity))
        {
            pending.push_back(child);
        }
    }
    return entities;
}

/* The decode's handle pairs, for the blob lists whose handles the component
   fields cannot expose. */
struct EntityRemap
{
    const std::vector<std::pair<ecs::Entity, ecs::Entity>>* pairs = nullptr;
};

ecs::Entity mapSavedEntity(void* user_data, ecs::Entity saved)
{
    auto* remap = static_cast<EntityRemap*>(user_data);
    for (const auto& [old, fresh] : *remap->pairs)
    {
        if (old == saved)
        {
            return fresh;
        }
    }
    return {};
}

std::string saveRoot(std::string& error)
{
    const std::string root = save::defaultUserDataRoot(kSaveApplication);
    if (root.empty())
    {
        error = "the platform names no application-data directory for saves";
    }
    return root;
}

} // namespace

bool saveScene(const ecs::World& world, const std::vector<ecs::Entity>& roots,
               const std::string& name, std::string& error)
{
    const std::string root = saveRoot(error);
    if (root.empty())
    {
        return false;
    }
    const std::vector<ecs::Entity> entities = sceneEntities(world, roots);
    const std::vector<ecs::ComponentIndex> opaque = opaqueComponents(world);
    save::SaveFile file;
    file.version = kSceneSaveVersion;
    if (!save::encodeWorld(world, contentComponents(),
                           Span<const ecs::Entity>(entities.data(), entities.size()),
                           Span<const ecs::ComponentIndex>(opaque.data(), opaque.size()),
                           file.payload, error))
    {
        return false;
    }
    save::SaveStore store(root);
    return store.write(name, file, error);
}

bool loadScene(ecs::World& world, const std::vector<ecs::Entity>& roots, const std::string& name,
               std::vector<ecs::Entity>& out_roots, std::string& error)
{
    const std::string root = saveRoot(error);
    if (root.empty())
    {
        return false;
    }
    save::SaveStore store(root);
    save::SaveFile file;
    if (!store.read(name, file, error))
    {
        return false;
    }
    if (file.version > kSceneSaveVersion)
    {
        error = "the save is version " + std::to_string(file.version) + "; this build reads up to "
                + std::to_string(kSceneSaveVersion);
        return false;
    }

    /* Decode beside the live scene: the decode rolls its own entities back on
       failure, so a corrupt or newer slot leaves the running scene untouched. */
    std::vector<ecs::Entity> restored;
    std::vector<std::pair<ecs::Entity, ecs::Entity>> remap;
    if (!save::decodeWorld(world, contentComponents(), contentMigrations(),
                           Span<const std::byte>(file.payload.data(), file.payload.size()), error,
                           &restored, &remap))
    {
        return false;
    }

    /* The hierarchy's child lists live in blobs, so the field remap cannot see
       their handles; rewrite them through the decode's pairs before the old
       scene goes away. */
    EntityRemap entity_remap{&remap};
    hierarchy::remapChildren(world, Span<const ecs::Entity>(restored.data(), restored.size()),
                             &entity_remap, &mapSavedEntity);

    for (const ecs::Entity entity : sceneEntities(world, roots))
    {
        if (world.alive(entity))
        {
            world.destroy(entity);
        }
    }

    out_roots.clear();
    for (const ecs::Entity entity : restored)
    {
        const auto* parent = world.get<hierarchy::Parent>(entity);
        if (parent == nullptr || !parent->parent.valid())
        {
            out_roots.push_back(entity);
        }
    }
    return true;
}

} // namespace resonate::startup
