#include <catch2/catch_all.hpp>

#include <cmath>
#include <string>

#include <resonate/animation/player.h>
#include <resonate/core/allocator.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>

namespace
{

using resonate::Vec3;
using resonate::animation::Clip;
using resonate::animation::QuatTrack;
using resonate::animation::Vec3Track;
using resonate::ecs::Entity;
using resonate::ecs::World;

Vec3Track positionsOf(std::initializer_list<Vec3Track::Key> keys)
{
    Vec3Track track;
    track.keys.assign(keys.begin(), keys.end());
    return track;
}

QuatTrack yawOf(std::initializer_list<QuatTrack::Key> keys)
{
    QuatTrack track;
    track.keys.assign(keys.begin(), keys.end());
    return track;
}

bool near(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.001F;
}

Clip walkClip()
{
    Clip clip;
    clip.duration = 2.0F;
    clip.position = positionsOf({{0.0F, Vec3{0.0F, 0.0F, 0.0F}}, {2.0F, Vec3{2.0F, 0.0F, 0.0F}}});
    clip.rotation = yawOf({{0.0F, resonate::quatFromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, 0.0F)},
                           {2.0F, resonate::quatFromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, 1.5707964F)}});
    clip.scale = positionsOf({{1.0F, Vec3{1.0F, 1.0F, 1.0F}}});
    return clip;
}

} // namespace

TEST_CASE("animation: a track interpolates, holds and wraps", "[animation]")
{
    Clip clip = walkClip();
    resonate::hierarchy::LocalTransform transform;

    /* Midway between the keys: position and yaw are half way. */
    clip.sample(1.0F, false, transform);
    CHECK(near(transform.position.x, 1.0F));
    CHECK(near(transform.rotation.y, std::sin(0.5F * 0.5F * 1.5707964F)));

    /* Before the first key holds it; after the last holds the end. */
    clip.sample(-1.0F, false, transform);
    CHECK(near(transform.position.x, 0.0F));
    clip.sample(9.0F, false, transform);
    CHECK(near(transform.position.x, 2.0F));

    /* A loop wraps: time 2.5 reads as 0.5. */
    clip.sample(2.5F, true, transform);
    CHECK(near(transform.position.x, 0.5F));

    /* A single-key track holds its value for the whole clip. */
    CHECK(near(transform.scale.x, 1.0F));
}

TEST_CASE("animation: the player advances states and writes the transform",
          "[animation]")
{
    World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(world));

    resonate::animation::AnimationPlayer player;
    const std::uint32_t clip = player.addClip(walkClip());

    const Entity entity = world.create();
    world.add(entity, resonate::hierarchy::LocalTransform{});
    world.add(entity, resonate::animation::AnimationState{clip});

    player.invoke(world, 1.0F);
    const auto* transform = world.get<resonate::hierarchy::LocalTransform>(entity);
    REQUIRE(transform != nullptr);
    CHECK(near(transform->position.x, 1.0F));
    const auto* state = world.get<resonate::animation::AnimationState>(entity);
    REQUIRE(state != nullptr);
    CHECK(near(state->time, 1.0F));

    /* A non-looping state stops at the clip's end and quits playing. */
    world.remove<resonate::animation::AnimationState>(entity);
    world.add(entity, resonate::animation::AnimationState{clip, 0.0F, 1.0F, false, true});
    player.invoke(world, 2.0F);
    player.invoke(world, 1.0F);
    CHECK(near(state->time, 2.0F));
    CHECK_FALSE(state->playing);
    CHECK(near(transform->position.x, 2.0F));

    /* A state naming a missing clip is skipped, not crashed. */
    const Entity stray = world.create();
    world.add(stray, resonate::hierarchy::LocalTransform{});
    world.add(stray, resonate::animation::AnimationState{99U});
    player.invoke(world, 1.0F);
    CHECK(near(world.get<resonate::animation::AnimationState>(stray)->time, 0.0F));
}

TEST_CASE("animation: two clips blend per field with the weight", "[animation]")
{
    Clip idle;
    idle.duration = 2.0F;
    idle.position = positionsOf({{0.0F, Vec3{0.0F, 0.0F, 0.0F}}, {2.0F, Vec3{2.0F, 0.0F, 0.0F}}});

    Clip run;
    run.duration = 2.0F;
    run.position = positionsOf({{0.0F, Vec3{0.0F, 1.0F, 0.0F}}, {2.0F, Vec3{2.0F, 3.0F, 0.0F}}});
    run.rotation = yawOf({{0.0F, resonate::quatFromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, 0.0F)}});

    resonate::hierarchy::LocalTransform transform;
    transform.scale = Vec3{2.0F, 2.0F, 2.0F};

    /* Both drive position: the halfway weight mixes them. At t=1 both x
       tracks read 1.0, and the y tracks differ by 2.0. */
    Clip::blend(idle, run, 0.5F, 1.0F, false, transform);
    CHECK(near(transform.position.x, 1.0F));
    CHECK(near(transform.position.y, 1.0F));

    /* Weight 0 answers from, 1 answers to. */
    Clip::blend(idle, run, 0.0F, 1.0F, false, transform);
    CHECK(near(transform.position.x, 1.0F));
    Clip::blend(idle, run, 1.0F, 1.0F, false, transform);
    CHECK(near(transform.position.y, 2.0F));

    /* A field only one clip drives comes whole; a field neither drives
       keeps what it had. */
    CHECK(near(transform.rotation.y, 0.0F));
    CHECK(near(transform.scale.x, 2.0F));
}

TEST_CASE("animation: a machine-driven entity cross-fades its two clips", "[animation]")
{
    World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(world));

    resonate::animation::AnimationPlayer player;
    /* Rise and fall share the same path; only their direction differs, so a
       blended transform reads back as a plain height. */
    Clip rise;
    rise.duration = 2.0F;
    rise.position.keys = {{0.0F, Vec3{}}, {2.0F, Vec3{0.0F, 2.0F, 0.0F}}};
    Clip fall;
    fall.duration = 2.0F;
    fall.position.keys = {{0.0F, Vec3{0.0F, 2.0F, 0.0F}}, {2.0F, Vec3{}}};

    resonate::animation::StateMachine machine;
    machine.states.push_back(resonate::animation::State{player.addClip(std::move(rise)), 2.0F,
                                                        false});
    machine.states.push_back(resonate::animation::State{player.addClip(std::move(fall)), 2.0F,
                                                        false});
    resonate::animation::Transition to_fall;
    to_fall.from = 0U;
    to_fall.to = 1U;
    to_fall.at_end_only = true;
    to_fall.fade_seconds = 1.0F;
    machine.transitions.push_back(to_fall);
    resonate::animation::Transition to_rise;
    to_rise.from = 1U;
    to_rise.to = 0U;
    to_rise.at_end_only = true;
    to_rise.fade_seconds = 1.0F;
    machine.transitions.push_back(to_rise);

    const Entity entity = world.create();
    world.add(entity, resonate::hierarchy::LocalTransform{});
    world.add(entity, resonate::animation::AnimationState{});
    world.add(entity, resonate::animation::MachineState{player.addMachine(std::move(machine)),
                                                        resonate::animation::MachineRuntime{}});

    const auto height = [&world, entity]()
    {
        const auto* transform = world.get<resonate::hierarchy::LocalTransform>(entity);
        REQUIRE(transform != nullptr);
        return transform->position.y;
    };
    auto* playback = world.get<resonate::animation::AnimationState>(entity);
    REQUIRE(playback != nullptr);

    /* The first frame enters the initial state and samples it. */
    player.invoke(world, 1.0F);
    CHECK(near(height(), 1.0F));
    CHECK(playback->driven);

    /* At the end of the rise the machine switches and starts its fade: the
       incoming weight is 0, so the outgoing clip is what paints. */
    player.invoke(world, 1.0F);
    CHECK(playback->clip == 1U);
    CHECK(playback->previous_clip == 0U);
    CHECK(near(playback->blend, 0.0F));
    CHECK(near(height(), 2.0F));

    /* Halfway through the fade the two samples mix: the outgoing holds 2.0,
       the incoming is at 0.5 s of its fall (1.5). */
    player.invoke(world, 0.5F);
    CHECK(near(playback->blend, 0.5F));
    CHECK(near(height(), 1.75F));

    /* The fade settles and the current clip alone paints. */
    player.invoke(world, 1.0F);
    CHECK(near(playback->blend, 1.0F));
    CHECK(near(height(), 0.5F));

    /* A paused state is not advanced by its machine either. */
    playback->playing = false;
    const float held = playback->time;
    const float kept = height();
    player.invoke(world, 1.0F);
    CHECK(near(playback->time, held));
    CHECK(near(height(), kept));
}

TEST_CASE("a clip parses from the .rclip XML subset", "[animation]")
{
    constexpr const char* kText = R"xml(<?xml version="1.0" encoding="utf-8"?>
<clip duration="1.5">
  <track field="position">
    <key t="0" value="0 0.5 0"/>
    <key t="0.75" value="0 1.1 0"/>
    <key t="1.5" value="0 0.5 0"/>
  </track>
  <track field="rotation">
    <key t="0" value="0 0 0 1"/>
  </track>
</clip>)xml";
    Clip clip;
    std::string error;
    REQUIRE(Clip::parse(kText, clip, error));
    CHECK(error.empty());
    CHECK(near(clip.duration, 1.5F));
    REQUIRE(clip.position.keys.size() == 3U);
    CHECK(near(clip.position.keys[1].value.y, 1.1F));
    REQUIRE(clip.rotation.keys.size() == 1U);

    /* The reader must not transpose a quat: the binder and the writer both use
       member order, and the scene writer's old w-first bug is the precedent. */
    resonate::hierarchy::LocalTransform transform;
    clip.sample(0.375F, true, transform);
    CHECK(near(transform.position.y, 0.8F));
    CHECK(near(transform.rotation.w, 1.0F));
    CHECK(near(transform.rotation.x, 0.0F));
    CHECK_FALSE(clip.position_root_motion);

    /* The position track can be flagged as a displacement curve. */
    REQUIRE(Clip::parse(R"xml(<clip duration="1">
  <track field="position" root_motion="true"><key t="0" value="0 0 0"/></track>
</clip>)xml", clip, error));
    CHECK(clip.position_root_motion);
}

TEST_CASE("a clip refuses what the grammar does not name", "[animation]")
{
    Clip clip;
    std::string error;
    CHECK_FALSE(Clip::parse("<clip/>", clip, error));
    CHECK(error.find("duration") != std::string::npos);

    CHECK_FALSE(Clip::parse(R"xml(<clip duration="1">
  <track field="yaw"><key t="0" value="0 0 0"/></track>
</clip>)xml", clip, error));
    CHECK(error.find("unknown field") != std::string::npos);

    CHECK_FALSE(Clip::parse(R"xml(<clip duration="1">
  <track field="position">
    <key t="0.5" value="0 0 0"/>
    <key t="0.25" value="0 0 0"/>
  </track>
</clip>)xml", clip, error));
    CHECK(error.find("ascend") != std::string::npos);

    CHECK_FALSE(Clip::parse(R"xml(<clip duration="1">
  <track field="position"><key t="0" value="0 0"/></track>
</clip>)xml", clip, error));
    CHECK(error.find("numbers") != std::string::npos);

    CHECK_FALSE(Clip::parse(R"xml(<clip duration="1"><beat/></clip>)xml", clip, error));
    CHECK(error.find("unknown element") != std::string::npos);

    CHECK_FALSE(Clip::parse(R"xml(<clip duration="1">
  <track field="position"><key t="2" value="0 0 0"/></track>
</clip>)xml", clip, error));
    CHECK(error.find("outside the clip") != std::string::npos);

    /* Errors carry the line the offender sits on. */
    CHECK_FALSE(Clip::parse("<clip duration=\"1\">\n  <beat/>\n</clip>", clip, error));
    CHECK(error.find(":2:") != std::string::npos);

    CHECK_FALSE(Clip::parse(R"xml(<clip duration="1">
  <track field="position" root_motion="maybe"><key t="0" value="0 0 0"/></track>
</clip>)xml", clip, error));
    CHECK(error.find("true or false") != std::string::npos);

    CHECK_FALSE(Clip::parse(R"xml(<clip duration="1">
  <track field="rotation" root_motion="true"><key t="0" value="0 0 0 1"/></track>
</clip>)xml", clip, error));
    CHECK(error.find("position track only") != std::string::npos);
}

TEST_CASE("animation: a root-motion clip moves the entity instead of its pose", "[animation]")
{
    Clip root;
    root.duration = 1.0F;
    root.position = positionsOf({{0.0F, Vec3{0.0F, 0.0F, 0.0F}}, {1.0F, Vec3{1.0F, 0.0F, 0.0F}}});
    root.position_root_motion = true;

    /* The pose keeps its position; the track is a displacement curve. */
    resonate::hierarchy::LocalTransform transform;
    transform.position = Vec3{5.0F, 0.0F, 0.0F};
    root.sample(0.5F, true, transform);
    CHECK(near(transform.position.x, 5.0F));
    CHECK(near(root.rootAt(0.5F).x, 0.5F));

    World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(world));

    resonate::animation::AnimationPlayer player;
    const std::uint32_t clip = player.addClip(root);
    resonate::hierarchy::LocalTransform start;
    start.position = Vec3{5.0F, 0.0F, 0.0F};
    const Entity entity = world.create();
    world.add(entity, start);
    world.add(entity, resonate::animation::AnimationState{clip});

    /* Five quarter steps cross the loop border: the displacement accumulates
       past one cycle instead of snapping back to the clip's start. */
    for (int step = 0; step < 5; ++step)
    {
        player.invoke(world, 0.25F);
    }
    const auto* moved = world.get<resonate::hierarchy::LocalTransform>(entity);
    REQUIRE(moved != nullptr);
    CHECK(near(moved->position.x, 6.25F));
    player.invoke(world, 0.2F);
    CHECK(near(moved->position.x, 6.45F));
}
