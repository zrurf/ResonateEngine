#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <resonate/core/allocator.h>
#include <resonate/csharp/bindings.h>
#include <resonate/csharp/dispatch.h>
#include <resonate/csharp/host.hpp>
#include <resonate/csharp/managed_systems.hpp>
#include <resonate/csharp/registration.h>

#include "resonate.render/components.gen.h"

#include "paths.h"

namespace
{

using AddFn = int (*)(int, int);
using RegisterFn = int (*)(void*);

/* The fixture is tests/csharp/managed; scripts/csharp_test.lua publishes it to
   build/tools/csharp-test before the test target builds. */
struct Fixture
{
    std::string directory;
    std::string config;
    std::string assembly;

    Fixture()
    {
        directory = resonate::test::findBuildDirectory("tools/csharp-test");
        config = directory + "/Resonate.Tests.Managed.runtimeconfig.json";
        assembly = directory + "/Resonate.Tests.Managed.dll";
    }
};

constexpr const char* kProbeType = "Resonate.Tests.Managed.Probe, Resonate.Tests.Managed";

} // namespace

/* The registration entry the managed side calls, holding the table in its
   opaque context. A managed function pointer arrives as void*, the same shape
   dlsym hands back. */
extern "C" std::int32_t registerEntry(void* context, std::uint32_t id, void* entry)
{
    auto* table = static_cast<resonate::csharp::DispatchTable*>(context);
    const auto function = reinterpret_cast<resonate::csharp::DispatchEntry>(entry);
    return table->set(id, function) ? 0 : 1;
}

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

    void* function = host.load_function(fixture.assembly, kProbeType, "Register", error);
    REQUIRE(function != nullptr);
    const auto register_probe = reinterpret_cast<RegisterFn>(function);

    resonate::csharp::DispatchTable table(16U);
    ResonateCSharpRegistration registration{&table, &registerEntry};
    REQUIRE(register_probe(&registration) == 0);

    int counters[2] = {0, -1};
    resonate::csharp::bindings::SystemContext context{};
    context.user = counters;
    context.frame_index = 5U;
    REQUIRE(table.get(resonate::csharp::bindings::kSystemTick) != nullptr);
    CHECK(table.invoke(resonate::csharp::bindings::kSystemTick, &context));
    CHECK(counters[0] == 1);
    CHECK(counters[1] == 5);
    CHECK(table.invoke(resonate::csharp::bindings::kSystemTick, &context));
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
    REQUIRE(systems.load(fixture.config, fixture.assembly, kProbeType, error));

    int counters[2] = {0, -1};
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
