#include <catch2/catch_all.hpp>

#include <cmath>

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
