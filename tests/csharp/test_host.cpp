#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <resonate/core/allocator.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/csharp/bindings.h>
#include <resonate/csharp/dispatch.h>
#include <resonate/csharp/host.hpp>
#include <resonate/csharp/managed_systems.hpp>

#include "resonate.render/components.gen.h"

#include "paths.h"

namespace
{

using AddFn = int (*)(int, int);
using RegisterFn = int (*)(void*);

/* The fixture's counter block; the managed tick documents the indices. */
constexpr int kCounterWords = 12;

/* The fixture is tests/csharp/managed; scripts/csharp_test.lua publishes it to
   build/tools/csharp-test before the test target builds. */
struct Fixture
{
    std::string directory;
    std::string config;
    std::string assembly;
    std::string bootstrap;
    std::string patch;

    Fixture()
    {
        directory = resonate::test::findBuildDirectory("tools/csharp-test");
        config = directory + "/Resonate.Tests.Managed.runtimeconfig.json";
        assembly = directory + "/Resonate.Tests.Managed.dll";
        const std::string bootstrap_dir =
            resonate::test::findBuildDirectory("tools/csharp-bootstrap");
        if (!bootstrap_dir.empty())
        {
            bootstrap = bootstrap_dir + "/Resonate.Tests.Bootstrap.dll";
        }
        const std::string patch_dir = resonate::test::findBuildDirectory("tools/csharp-patch");
        if (!patch_dir.empty())
        {
            patch = patch_dir + "/Resonate.Tests.Patch.dll";
        }
    }
};

constexpr const char* kProbeType = "Resonate.Tests.Managed.Probe, Resonate.Tests.Managed";
/* The type the registration generator emits: the fixture's unmanaged entry and
   its reflect-callable Register live here. */
constexpr const char* kRegistrationType =
    "Resonate.CSharp.Generated.BehaviourRegistration, Resonate.Tests.Managed";
constexpr const char* kPatchRegistrationType =
    "Resonate.CSharp.Generated.BehaviourRegistration";
constexpr const char* kBootstrapType =
    "Resonate.Tests.Bootstrap.PatchHost, Resonate.Tests.Bootstrap";

} // namespace

/* The registration entry the managed side calls, holding the table in its
   opaque context. A managed function pointer arrives as void*, the same shape
   dlsym hands back. The stage rides along; this table serves all of them. */
extern "C" std::int32_t registerEntry(void* context, std::uint32_t stage, std::uint32_t id,
                                      void* entry)
{
    (void)stage;
    auto* table = static_cast<resonate::csharp::DispatchTable*>(context);
    const auto function = reinterpret_cast<resonate::csharp::DispatchEntry>(entry);
    return table->set(id, function) ? 0 : 1;
}

/* The patch path registers through replace: the slot is already taken. */
extern "C" std::int32_t replaceEntry(void* context, std::uint32_t stage, std::uint32_t id,
                                     void* entry)
{
    (void)stage;
    auto* table = static_cast<resonate::csharp::DispatchTable*>(context);
    const auto function = reinterpret_cast<resonate::csharp::DispatchEntry>(entry);
    return table->replace(id, function) ? 0 : 1;
}

/* The pre-patch entry, so the swap is observable. */
void v1Tick(void* context)
{
    auto* frame = static_cast<resonate::csharp::bindings::SystemContext*>(context);
    auto* counters = static_cast<int*>(frame->user);
    counters[0] += 1;
    counters[1] = 1;
}

/* Mirrors the bootstrap's InstallRequest. */
struct InstallRequest
{
    const char* path;
    const char* entry_type;
    void* registration;
    void* context;
};

TEST_CASE("csharp: hostfxr starts the runtime and calls a managed method", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::RuntimeHost host;
    std::string error;
    REQUIRE(host.initialize(fixture.config, error));

    void* function = host.load_function(fixture.assembly, kProbeType, "Add", error);
    REQUIRE(function != nullptr);

    const auto add = reinterpret_cast<AddFn>(function);
    CHECK(add(20, 22) == 42);
    CHECK(add(-5, 5) == 0);
}

TEST_CASE("csharp: an unknown managed method reports a named error", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::RuntimeHost host;
    std::string error;
    REQUIRE(host.initialize(fixture.config, error));

    void* function = host.load_function(fixture.assembly, kProbeType, "Missing", error);
    CHECK(function == nullptr);
    CHECK(!error.empty());
}

TEST_CASE("csharp: managed code registers an entry into the dispatch table", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::RuntimeHost host;
    std::string error;
    REQUIRE(host.initialize(fixture.config, error));

    void* function =
        host.load_function(fixture.assembly, kRegistrationType, "RegisterUnmanaged", error);
    REQUIRE(function != nullptr);
    const auto register_probe = reinterpret_cast<RegisterFn>(function);

    resonate::csharp::DispatchTable table(32U);
    resonate::csharp::bindings::Registration registration{&table, &registerEntry};
    REQUIRE(register_probe(&registration) == 0);

    int counters[kCounterWords] = {0, -1, 0, 0, -1, 0, 0, 0};
    resonate::csharp::bindings::SystemContext context{};
    context.user = counters;
    context.frame_index = 5U;
    REQUIRE(table.get(resonate::csharp::bindings::kSystemUpdate) != nullptr);
    CHECK(table.get(resonate::csharp::bindings::kSystemLateUpdate) != nullptr);
    CHECK(table.invoke(resonate::csharp::bindings::kSystemUpdate, &context));
    CHECK(counters[0] == 1);
    CHECK(counters[1] == 5);
    CHECK(table.invoke(resonate::csharp::bindings::kSystemUpdate, &context));
    CHECK(counters[0] == 2);
}

TEST_CASE("csharp: a generated view struct reads as a span on the managed side", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::RuntimeHost host;
    std::string error;
    REQUIRE(host.initialize(fixture.config, error));

    void* verify = host.load_function(fixture.assembly, kProbeType, "VerifyFrameViewLayout", error);
    REQUIRE(verify != nullptr);
    CHECK(reinterpret_cast<int (*)()>(verify)() == 0);

    void* sum_function = host.load_function(fixture.assembly, kProbeType, "SumTransforms", error);
    REQUIRE(sum_function != nullptr);

    void* read_handle = host.load_function(fixture.assembly, kProbeType, "ReadHandle", error);
    REQUIRE(read_handle != nullptr);

    const float transforms[3] = {1.5F, 2.5F, 3.0F};
    resonate::csharp::bindings::FrameView view{};
    view.handle.index = 1U;
    view.handle.generation = 2U;
    view.delta_seconds = 1.0F / 60.0F;
    view.frame_index = 3U;
    view.transform_data = transforms;
    view.transform_count = 3U;

    const auto sum = reinterpret_cast<float (*)(void*)>(sum_function);
    CHECK(sum(&view) == 7.0F);

    const auto handle = reinterpret_cast<std::uint64_t (*)(void*)>(read_handle);
    CHECK(handle(&view) == 0x0000000200000001ULL);
}

TEST_CASE("csharp: schema-generated component views read native chunks", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::RuntimeHost host;
    std::string error;
    REQUIRE(host.initialize(fixture.config, error));

    void* verify = host.load_function(fixture.assembly, kProbeType, "VerifySchemaLayouts", error);
    REQUIRE(verify != nullptr);
    CHECK(reinterpret_cast<int (*)()>(verify)() == 0);

    void* sum_function = host.load_function(fixture.assembly, kProbeType, "SumFarZ", error);
    REQUIRE(sum_function != nullptr);

    resonate::render::Camera cameras[2] = {};
    cameras[0].far_z = 100.0F;
    cameras[1].far_z = 250.0F;

    const auto sum = reinterpret_cast<float (*)(void*, int)>(sum_function);
    CHECK(sum(cameras, 2) == 350.0F);
}

TEST_CASE("csharp: the frame loop calls a managed system through the table", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::ManagedSystems systems;
    std::string error;
    REQUIRE(systems.load(fixture.config, fixture.assembly, kRegistrationType, error));

    int counters[kCounterWords] = {0, -1, 0, 0, -1, 0};
    systems.set_user(counters);

    resonate::ecs::World world(resonate::systemAllocator());
    resonate::ecs::CommandBuffer commands(world);
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.25F);
    CHECK(counters[0] == 1);
    CHECK(counters[1] == 0);
    CHECK(systems.frames() == 1U);

    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.25F);
    CHECK(counters[0] == 2);
    CHECK(counters[1] == 1);
    CHECK(systems.frames() == 2U);
}

TEST_CASE("csharp: a managed system reads a named component's chunk", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::ManagedSystems systems;
    std::string error;
    REQUIRE(systems.load(fixture.config, fixture.assembly, kRegistrationType, error));
    systems.set_component("resonate.hierarchy.LocalTransform");

    int counters[kCounterWords] = {0, -1, 0, 0, -1, 0};
    systems.set_user(counters);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    const resonate::ecs::Entity first = world.create();
    world.add(first, resonate::hierarchy::LocalTransform{
                         resonate::Vec3{1.0F, 0.0F, 0.0F}, resonate::Quat{},
                         resonate::Vec3{1.0F, 1.0F, 1.0F}});
    const resonate::ecs::Entity second = world.create();
    world.add(second, resonate::hierarchy::LocalTransform{
                          resonate::Vec3{2.0F, 0.0F, 0.0F}, resonate::Quat{},
                          resonate::Vec3{1.0F, 1.0F, 1.0F}});

    struct Observed
    {
        int runs = 0;
    };
    Observed observed;
    REQUIRE(world.addObserver<resonate::hierarchy::LocalTransform>(
        &observed,
        [](void* user, resonate::ecs::World&, resonate::ecs::Entity,
           resonate::ecs::ComponentIndex) { ++static_cast<Observed*>(user)->runs; }));

    resonate::ecs::CommandBuffer commands(world);
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    CHECK(counters[0] == 1);
    CHECK(counters[1] == 0);
    CHECK(counters[2] == 30);
    CHECK(counters[3] == 2);
    CHECK(counters[4] == static_cast<int>(first.index));
    CHECK(counters[5] == 0);
    CHECK(counters[11] == 1);
    CHECK(systems.frames() == 1U);

    /* The managed write landed and the mark made the observers see it. */
    CHECK(world.get<resonate::hierarchy::LocalTransform>(first)->position.y == 1.0F);
    CHECK(world.get<resonate::hierarchy::LocalTransform>(second)->position.y == 1.0F);
    CHECK(observed.runs == 2);
}

TEST_CASE("csharp: a large chunk set reaches managed code in one frame", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::ManagedSystems systems;
    std::string error;
    REQUIRE(systems.load(fixture.config, fixture.assembly, kRegistrationType, error));
    systems.set_component("resonate.hierarchy.LocalTransform");

    int counters[kCounterWords] = {0, -1, 0, 0, -1, 0};
    systems.set_user(counters);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    constexpr int kEntities = 100000;
    for (int index = 0; index < kEntities; ++index)
    {
        const resonate::ecs::Entity entity = world.create();
        world.add(entity, resonate::hierarchy::LocalTransform{
                              resonate::Vec3{1.0F, 0.0F, 0.0F}, resonate::Quat{},
                              resonate::Vec3{1.0F, 1.0F, 1.0F}});
    }

    resonate::ecs::CommandBuffer commands(world);
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);

    CHECK(counters[1] == 0);
    CHECK(counters[3] == kEntities);
    CHECK(counters[2] == kEntities * 10);
    CHECK(counters[0] > 1);
    CHECK(systems.frames() == 1U);
}

TEST_CASE("csharp: a managed spawn plays through the command buffer", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::ManagedSystems systems;
    std::string error;
    REQUIRE(systems.load(fixture.config, fixture.assembly, kRegistrationType, error));
    systems.set_component("resonate.hierarchy.LocalTransform");

    /* counters[6] = 1 asks the managed tick to spawn once on frame 0. */
    int counters[kCounterWords] = {0, -1, 0, 0, -1, 0, 1};
    systems.set_user(counters);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    const resonate::ecs::Entity existing = world.create();
    world.add(existing, resonate::hierarchy::LocalTransform{
                            resonate::Vec3{1.0F, 0.0F, 0.0F}, resonate::Quat{},
                            resonate::Vec3{1.0F, 1.0F, 1.0F}});

    resonate::ecs::CommandBuffer commands(world);
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    CHECK(counters[6] == 0);
    CHECK(counters[7] != 0);

    /* What the managed system recorded becomes alive at the frame's sync
       point, not during the tick. */
    world.play(commands);

    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    CHECK(counters[3] == 3);

    const resonate::ecs::ComponentIndex all[] = {
        resonate::ecs::ComponentTraits<resonate::hierarchy::LocalTransform>::index};
    resonate::ecs::QueryDesc desc;
    desc.all = resonate::Span<const resonate::ecs::ComponentIndex>(all, 1U);
    resonate::ecs::Query query = world.createQuery(desc);
    bool found = false;
    query.forEachChunk(
        [](void* context, resonate::ecs::ChunkView view)
        {
            const auto* data = static_cast<const resonate::hierarchy::LocalTransform*>(
                view.componentData(
                    resonate::ecs::ComponentTraits<resonate::hierarchy::LocalTransform>::index));
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                /* The managed tick wrote Y on every row; X and Z are the
                   spawned values. */
                if (data[row].position.x == 4.0F && data[row].position.z == 6.0F)
                {
                    *static_cast<bool*>(context) = true;
                }
            }
        },
        &found);
    CHECK(found);
}

TEST_CASE("csharp: a managed remove plays through the command buffer", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::ManagedSystems systems;
    std::string error;
    REQUIRE(systems.load(fixture.config, fixture.assembly, kRegistrationType, error));
    systems.set_component("resonate.hierarchy.LocalTransform");

    /* The tick spawns on frame 0; the request word makes frame 1 remove the
       spawned component through the generated commands. */
    int counters[kCounterWords] = {0, -1, 0, 0, -1, 0, 1};
    systems.set_user(counters);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    resonate::ecs::CommandBuffer commands(world);

    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    REQUIRE(counters[6] == 0);
    world.play(commands);

    counters[8] = 1;
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    CHECK(counters[9] == 1);
    world.play(commands);

    /* The entity stays alive; only its component is gone. */
    const resonate::ecs::Entity spawned{static_cast<std::uint32_t>(counters[7]),
                                        static_cast<std::uint32_t>(counters[10])};
    CHECK(world.alive(spawned));
    CHECK(world.get<resonate::hierarchy::LocalTransform>(spawned) == nullptr);
}

TEST_CASE("csharp: a managed despawn plays through the command buffer", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::ManagedSystems systems;
    std::string error;
    REQUIRE(systems.load(fixture.config, fixture.assembly, kRegistrationType, error));
    systems.set_component("resonate.hierarchy.LocalTransform");

    int counters[kCounterWords] = {0, -1, 0, 0, -1, 0, 1};
    systems.set_user(counters);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    resonate::ecs::CommandBuffer commands(world);

    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    REQUIRE(counters[6] == 0);
    world.play(commands);

    counters[8] = 2;
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    CHECK(counters[9] == 1);
    world.play(commands);

    const resonate::ecs::Entity spawned{static_cast<std::uint32_t>(counters[7]),
                                        static_cast<std::uint32_t>(counters[10])};
    CHECK_FALSE(world.alive(spawned));
}

TEST_CASE("csharp: a managed read sees another entity's component", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::ManagedSystems systems;
    std::string error;
    REQUIRE(systems.load(fixture.config, fixture.assembly, kRegistrationType, error));
    systems.set_component("resonate.hierarchy.LocalTransform");

    int counters[kCounterWords] = {0, -1, 0, 0, -1, 0};
    systems.set_user(counters);

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    const resonate::ecs::Entity first = world.create();
    world.add(first, resonate::hierarchy::LocalTransform{
                         resonate::Vec3{1.0F, 0.0F, 0.0F}, resonate::Quat{},
                         resonate::Vec3{1.0F, 1.0F, 1.0F}});
    const resonate::ecs::Entity second = world.create();
    world.add(second, resonate::hierarchy::LocalTransform{
                          resonate::Vec3{2.5F, 0.0F, 0.0F}, resonate::Quat{},
                          resonate::Vec3{1.0F, 1.0F, 1.0F}});

    resonate::ecs::CommandBuffer commands(world);
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);

    /* Frame 1 asks for a read of the first row through Commands; the read-only
       view returns the committed value. */
    counters[8] = 3;
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    CHECK(counters[9] == 10);
}

TEST_CASE("csharp: an entry runs only in the stage it declares", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());

    resonate::csharp::ManagedSystems systems;
    std::string error;
    REQUIRE(systems.load(fixture.config, fixture.assembly, kRegistrationType, error));
    CHECK(systems.usesStage(RESONATE_STAGE_UPDATE));
    CHECK(systems.usesStage(RESONATE_STAGE_LATE_UPDATE));
    CHECK_FALSE(systems.usesStage(RESONATE_STAGE_RENDER));

    resonate::ecs::World world(resonate::systemAllocator());
    resonate::ecs::CommandBuffer commands(world);
    int counters[kCounterWords] = {0, -1, 0, 0, -1, 0, 0, 0};
    systems.set_user(counters);

    resonate::csharp::ManagedSystems::invokeStage(RESONATE_STAGE_LATE_UPDATE, &systems, world,
                                                  commands, 0.1F);
    CHECK(counters[0] == 10);
    CHECK(counters[1] == 3);

    /* An empty stage is a no-op, not an error: the assembly need not fill
       every stage it could declare. */
    resonate::csharp::ManagedSystems::invokeStage(RESONATE_STAGE_RENDER, &systems, world,
                                                  commands, 0.1F);
    CHECK(counters[0] == 10);

    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.1F);
    CHECK(counters[0] == 11);
}

TEST_CASE("csharp: a collectible ALC patch replaces a dispatch slot", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());
    REQUIRE(!fixture.bootstrap.empty());
    REQUIRE(!fixture.patch.empty());

    resonate::csharp::RuntimeHost host;
    std::string error;
    REQUIRE(host.initialize(fixture.config, error));

    resonate::csharp::DispatchTable table(64U);
    REQUIRE(table.set(resonate::csharp::bindings::kSystemUpdate, &v1Tick));

    resonate::csharp::bindings::SystemContext context{};
    int before[kCounterWords] = {0, 0};
    context.user = before;
    REQUIRE(table.invoke(resonate::csharp::bindings::kSystemUpdate, &context));
    CHECK(before[1] == 1);

    void* install = host.load_function(fixture.bootstrap, kBootstrapType, "Install", error);
    REQUIRE(install != nullptr);

    /* The patch assembly loads into its own collectible context and registers
       through replace; the engine keeps only the function pointer. */
    resonate::csharp::bindings::Registration registration{&table, &replaceEntry};
    InstallRequest request{fixture.patch.c_str(), kPatchRegistrationType,
                           &registration, nullptr};
    CHECK(reinterpret_cast<int (*)(void*)>(install)(&request) == 0);
    CHECK(request.context != nullptr);

    int after[kCounterWords] = {0, 0};
    context.user = after;
    REQUIRE(table.invoke(resonate::csharp::bindings::kSystemUpdate, &context));
    CHECK(after[1] == 2);

    /* The slot is never called again after this: it pointed into the context
       the unload frees. */
    void* unload = host.load_function(fixture.bootstrap, kBootstrapType, "Unload", error);
    REQUIRE(unload != nullptr);
    CHECK(reinterpret_cast<int (*)(void*)>(unload)(request.context) == 0);
}

TEST_CASE("csharp: ManagedSystems installs and restores a patch", "[csharp]")
{
    const Fixture fixture;
    REQUIRE(!fixture.directory.empty());
    REQUIRE(!fixture.bootstrap.empty());
    REQUIRE(!fixture.patch.empty());

    resonate::csharp::ManagedSystems systems;
    std::string error;
    REQUIRE(systems.load(fixture.config, fixture.assembly, kRegistrationType, error));

    resonate::ecs::World world(resonate::systemAllocator());
    resonate::ecs::CommandBuffer commands(world);

    int base_counters[kCounterWords] = {0, -1, 0, 0, -1, 0};
    systems.set_user(base_counters);
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    CHECK(base_counters[0] == 1);
    CHECK(base_counters[2] == -1);

    REQUIRE(systems.load_patch(fixture.bootstrap, kBootstrapType, fixture.patch,
                               kPatchRegistrationType, error));
    CHECK(systems.patched());

    int patched[kCounterWords] = {0, -1, 0, 0, -1, 0};
    systems.set_user(patched);
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    CHECK(patched[0] == 1);
    CHECK(patched[1] == 2);
    CHECK(patched[2] == 0);

    REQUIRE(systems.unload_patch(error));
    CHECK_FALSE(systems.patched());

    /* The displaced entry is back: the base tick runs again. */
    int restored[kCounterWords] = {0, -1, 0, 0, -1, 0};
    systems.set_user(restored);
    resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    CHECK(restored[0] == 1);
    CHECK(restored[2] == -1);
}
