#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <resonate/csharp/bindings.h>
#include <resonate/csharp/dispatch.h>
#include <resonate/csharp/host.hpp>
#include <resonate/csharp/registration.h>

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

    int counter = 0;
    REQUIRE(table.get(resonate::csharp::bindings::kSystemTick) != nullptr);
    CHECK(table.invoke(resonate::csharp::bindings::kSystemTick, &counter));
    CHECK(counter == 1);
    CHECK(table.invoke(resonate::csharp::bindings::kSystemTick, &counter));
    CHECK(counter == 2);
}
