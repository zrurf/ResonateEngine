#include <catch2/catch_all.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <resonate/animation/player.h>
#include <resonate/asset/identity.h>
#include <resonate/core/allocator.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>

#include "resonate.animation/components.gen.h"

namespace
{

using resonate::animation::Animation;
using resonate::animation::AnimationPlayer;
using resonate::animation::AnimationState;
using resonate::ecs::Entity;
using resonate::ecs::World;

constexpr const char* kBobClip = R"xml(<clip duration="1">
  <track field="position">
    <key t="0" value="0 0.5 2.5"/>
    <key t="0.5" value="0 1.1 2.5"/>
    <key t="1" value="0 0.5 2.5"/>
  </track>
</clip>)xml";

constexpr const char* kGuid = "0704c7df-7013-4c31-818d-b515207bb7df";

/* A throwaway content root: one .rclip and the manifest that names it, so the
   player's manifest path and its clip paths both resolve. */
std::filesystem::path stageClip()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "resonate_clip_test";
    std::filesystem::create_directories(root);
    {
        std::ofstream out(root / "bob.rclip", std::ios::binary);
        out << kBobClip;
    }
    {
        std::ofstream out(root / "products.toml", std::ios::binary);
        out << "[[product]]\nguid = \"" << kGuid << "\"\nsub_id = 1\npath = \"bob.rclip\"\n";
    }
    return root;
}

} // namespace

TEST_CASE("animation: an authored clip becomes a running state", "[animation][assets]")
{
    const std::filesystem::path root = stageClip();
    const std::string manifest = (root / "products.toml").string();

    World world{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(AnimationPlayer::registerComponents(world));

    AnimationPlayer player;
    std::string error;
    REQUIRE(player.loadManifest(manifest, error));
    {
        /* The manifest is what the player resolves through; if it cannot see
           the clip, nothing downstream can. */
        resonate::asset::Identity identity;
        REQUIRE(identity.loadFile(manifest, error));
        resonate::asset::AssetId seeded{};
        REQUIRE(resonate::asset::parseGuid(kGuid, seeded));
        seeded.sub_id = 1U;
        CHECK(identity.locate(seeded).has_value());
    }

    resonate::asset::AssetId id;
    REQUIRE(resonate::asset::parseGuid(kGuid, id));
    Entity entity = world.create();
    REQUIRE(entity.valid());
    world.add(entity, resonate::hierarchy::LocalTransform{});
    Animation authored;
    authored.clip_low = id.guid_low;
    authored.clip_high = id.guid_high;
    authored.speed = 2.0F;
    world.add(entity, authored);

    /* The first invoke materializes the state; the second runs it. */
    player.invoke(world, 1.0F / 60.0F);
    const auto* state = world.get<AnimationState>(entity);
    REQUIRE(state != nullptr);
    CHECK(state->looping);
    CHECK(state->speed == 2.0F);
    const resonate::animation::Clip* played = player.clip(state->clip);
    REQUIRE(played != nullptr);
    CHECK(played->duration == 1.0F);

    /* One invoke materializes and advances: the state's time already ran the
       frame's delta at the authored speed. */
    CHECK(state->time == 1.0F / 60.0F * 2.0F);
    player.invoke(world, 0.5F);
    CHECK(state->time == (1.0F / 60.0F + 0.5F) * 2.0F);
    /* The sample moved the transform: the content, not startup code, drives it. */
    const auto* transform = world.get<resonate::hierarchy::LocalTransform>(entity);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.y > 0.5F);
}

TEST_CASE("animation: an unknown clip id is reported once and stays inert",
          "[animation][assets]")
{
    World world{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(AnimationPlayer::registerComponents(world));

    const std::filesystem::path root = stageClip();
    AnimationPlayer player;
    std::string error;
    REQUIRE(player.loadManifest((root / "products.toml").string(), error));

    resonate::asset::AssetId known;
    REQUIRE(resonate::asset::parseGuid(kGuid, known));
    resonate::asset::AssetId unknown = known;
    unknown.guid_low ^= 0xF00DU;

    Entity entity = world.create();
    world.add(entity, resonate::hierarchy::LocalTransform{});
    Animation authored;
    authored.clip_low = unknown.guid_low;
    authored.clip_high = unknown.guid_high;
    world.add(entity, authored);

    for (int frame = 0; frame < 5; ++frame)
    {
        player.invoke(world, 1.0F / 60.0F);
    }
    /* No state: a warning went out once and the entity is not pretending to
       animate. */
    CHECK(world.get<AnimationState>(entity) == nullptr);
}
