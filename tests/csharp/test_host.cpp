#include <catch2/catch_test_macros.hpp>

#include <string>

#include <resonate/csharp/host.hpp>

#include "paths.h"

namespace
{

using AddFn = int (*)(int, int);

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
