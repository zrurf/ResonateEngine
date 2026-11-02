#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <resonate/animation/player.h>
#include <resonate/application/save.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/name.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/renderer/renderer.h>
#include <resonate/startup/startup.hpp>
#include <resonate/ui/layout.h>

#include "resonate.gameplay/components.gen.h"

namespace
{

using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;

#if defined(_WIN32)
void setEnvironment(const char* name, const std::string& value)
{
    _putenv_s(name, value.c_str());
}

void clearEnvironment(const char* name)
{
    _putenv_s(name, "");
}
#else
void setEnvironment(const char* name, const std::string& value)
{
    setenv(name, value.c_str(), 1);
}

void clearEnvironment(const char* name)
{
    unsetenv(name);
}
#endif

/* The save root is a process environment variable; a case redirects it to a
   scratch directory and restores whatever was there before. */
struct UserData
{
    std::filesystem::path path;
    std::optional<std::string> previous;

    explicit UserData(const char* name)
        : path(std::filesystem::temp_directory_path() / ("resonate_savegame_" + std::string(name)))
    {
        if (const char* staged = std::getenv("RESONATE_USER_DATA"); staged != nullptr)
        {
            previous = staged;
        }
        std::error_code code;
        std::filesystem::remove_all(path, code);
        std::filesystem::create_directories(path, code);
        setEnvironment("RESONATE_USER_DATA", path.string());
    }

    ~UserData()
    {
        if (previous.has_value())
        {
            setEnvironment("RESONATE_USER_DATA", *previous);
        }
        else
        {
            clearEnvironment("RESONATE_USER_DATA");
        }
        std::error_code code;
        std::filesystem::remove_all(path, code);
    }

    UserData(const UserData&) = delete;
    UserData& operator=(const UserData&) = delete;
};

struct Scene
{
    World world{resonate::systemAllocator()};
    Entity root{};
    Entity child{};
    Entity outsider{};
    std::string error;

    Scene()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::animation::AnimationPlayer::registerComponents(world));
        REQUIRE(world.registerComponent<resonate::gameplay::Health>() !=
                resonate::ecs::kInvalidComponent);
        root = world.create();
        child = world.create();
        outsider = world.create();
        REQUIRE(root.valid());
        REQUIRE(child.valid());
        REQUIRE(outsider.valid());

        resonate::hierarchy::LocalTransform transform;
        transform.position = {1.0F, 2.0F, 3.0F};
        REQUIRE(world.add(root, transform) != nullptr);
        REQUIRE(world.add(outsider, transform) != nullptr);
        REQUIRE(resonate::hierarchy::setName(world, root, "root", error));

        resonate::gameplay::Health health;
        health.current = 42;
        health.max = 60;
        health.shield = 5;
        health.source = root;
        REQUIRE(world.add(child, health) != nullptr);

        /* The player's runtime state is opaque: the snapshot copies its bytes,
           because its indices name this process's clip and machine tables. */
        resonate::animation::AnimationState playback;
        playback.clip = 3U;
        playback.time = 1.25F;
        playback.blend = 0.5F;
        REQUIRE(world.add(child, playback) != nullptr);
        resonate::animation::MachineState machine;
        machine.machine = 2U;
        machine.runtime.current = 1U;
        machine.runtime.time = 0.75F;
        machine.runtime.values[0] = 1.0F;
        REQUIRE(world.add(child, machine) != nullptr);

        CommandBuffer commands(world);
        resonate::hierarchy::setParent(commands, child, root);
        world.play(commands);
    }
};

} // namespace

TEST_CASE("savegame: the scene round trips and leaves the host's entities alone", "[savegame]")
{
    UserData data("roundtrip");
    Scene scene;

    REQUIRE(resonate::startup::saveScene(scene.world, {scene.root}, "slot", scene.error));

    /* Change the live values, then load over them. */
    REQUIRE(scene.world.get<resonate::gameplay::Health>(scene.child) != nullptr);
    scene.world.get<resonate::gameplay::Health>(scene.child)->current = 1;

    std::vector<Entity> roots;
    REQUIRE(resonate::startup::loadScene(scene.world, {scene.root}, "slot", roots, scene.error));
    REQUIRE(roots.size() == 1U);
    CHECK(scene.world.alive(roots[0]));
    CHECK_FALSE(scene.world.alive(scene.root));
    CHECK(scene.world.alive(scene.outsider));

    CHECK(resonate::hierarchy::nameOf(scene.world, roots[0]) == "root");
    const auto* transform = scene.world.get<resonate::hierarchy::LocalTransform>(roots[0]);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.x == 1.0F);
    CHECK(transform->position.z == 3.0F);

    const resonate::Span<const Entity> children = resonate::hierarchy::children(scene.world, roots[0]);
    REQUIRE(children.size() == 1U);
    const Entity restored_child = children[0];
    const auto* health = scene.world.get<resonate::gameplay::Health>(restored_child);
    REQUIRE(health != nullptr);
    CHECK(health->current == 42);
    CHECK(health->shield == 5);
    CHECK(health->source == roots[0]);

    const auto* playback = scene.world.get<resonate::animation::AnimationState>(restored_child);
    REQUIRE(playback != nullptr);
    CHECK(playback->clip == 3U);
    CHECK(playback->time == 1.25F);
    CHECK(playback->blend == 0.5F);
    const auto* machine = scene.world.get<resonate::animation::MachineState>(restored_child);
    REQUIRE(machine != nullptr);
    CHECK(machine->machine == 2U);
    CHECK(machine->runtime.current == 1U);
    CHECK(machine->runtime.time == 0.75F);
    CHECK(machine->runtime.values[0] == 1.0F);
}

TEST_CASE("savegame: a scene hosted outside the save leaves the host's list clean", "[savegame]")
{
    UserData data("host");
    Scene scene;

    /* The UI tree hosts the scene root: alive across the load, not part of
       the save. */
    const Entity host = scene.world.create();
    REQUIRE(host.valid());
    CommandBuffer commands(scene.world);
    resonate::hierarchy::setParent(commands, scene.root, host);
    scene.world.play(commands);
    REQUIRE(resonate::hierarchy::childCount(scene.world, host) == 1U);

    REQUIRE(resonate::startup::saveScene(scene.world, {scene.root}, "slot", scene.error));
    std::vector<Entity> roots;
    REQUIRE(resonate::startup::loadScene(scene.world, {scene.root}, "slot", roots, scene.error));

    CHECK(scene.world.alive(host));
    /* The replaced scene left no dead handle in the host's child list. */
    CHECK(resonate::hierarchy::childCount(scene.world, host) == 0U);
    REQUIRE(roots.size() == 1U);
    CHECK(scene.world.alive(roots[0]));

    /* Re-attaching the restored scene is the caller's job; the list takes it. */
    CommandBuffer reattach(scene.world);
    resonate::hierarchy::setParent(reattach, roots[0], host);
    scene.world.play(reattach);
    REQUIRE(resonate::hierarchy::childCount(scene.world, host) == 1U);
    CHECK(resonate::hierarchy::children(scene.world, host)[0] == roots[0]);
}

TEST_CASE("savegame: a damaged or newer slot leaves the live scene untouched", "[savegame]")
{
    UserData data("damage");
    Scene scene;
    REQUIRE(resonate::startup::saveScene(scene.world, {scene.root}, "slot", scene.error));

    resonate::save::SaveStore store(data.path.string());
    resonate::save::SaveFile file;
    REQUIRE(store.read("slot", file, scene.error));
    REQUIRE_FALSE(file.payload.empty());
    file.payload[0] = static_cast<std::byte>(std::to_integer<std::uint8_t>(file.payload[0]) ^ 0x01U);
    REQUIRE(store.write("slot", file, scene.error));

    std::vector<Entity> roots;
    CHECK_FALSE(resonate::startup::loadScene(scene.world, {scene.root}, "slot", roots,
                                             scene.error));
    CHECK(scene.world.alive(scene.root));
    CHECK(scene.world.alive(scene.child));
    CHECK(roots.empty());

    /* A slot from a newer build is refused before the scene is touched. */
    scene.error.clear();
    REQUIRE(store.read("slot", file, scene.error));
    file.version = 99U;
    REQUIRE(store.write("slot", file, scene.error));
    CHECK_FALSE(resonate::startup::loadScene(scene.world, {scene.root}, "slot", roots,
                                             scene.error));
    CHECK(scene.error.find("version") != std::string::npos);
    CHECK(scene.world.alive(scene.root));
    CHECK(scene.world.alive(scene.child));
}

TEST_CASE("savegame: a missing slot is a read failure, not a scene teardown", "[savegame]")
{
    UserData data("missing");
    Scene scene;

    std::vector<Entity> roots;
    CHECK_FALSE(resonate::startup::loadScene(scene.world, {scene.root}, "nope", roots,
                                             scene.error));
    CHECK_FALSE(scene.error.empty());
    CHECK(scene.world.alive(scene.root));
    CHECK(scene.world.alive(scene.child));
}

TEST_CASE("savegame: the run's registered vocabulary is saveable", "[savegame]")
{
    UserData data("vocabulary");
    World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(world));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));
    const Entity root = world.create();
    REQUIRE(root.valid());

    std::string error;
    /* Every registered type is either schema-described or an opaque runtime
       component; a new hand-written component without either fails here. */
    const bool saved = resonate::startup::saveScene(world, {root}, "slot", error);
    INFO(error);
    CHECK(saved);
}
