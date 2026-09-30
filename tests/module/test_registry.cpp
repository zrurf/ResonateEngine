#include <catch2/catch_all.hpp>

#include <resonate/module/capability.h>

#include "fake_host.h"

namespace
{

struct FakeCapability
{
    ResonateCapabilityHeader header;
    int32_t value;
};

ResonateCapabilityRecord recordFor(const char* name, std::uint32_t version,
                                   FakeCapability& instance)
{
    ResonateCapabilityRecord record = {};
    record.id = resonate_id_make(name);
    record.version = version;
    record.instance = &instance;
    record.struct_size = sizeof(FakeCapability);
    record.name = name;
    return record;
}

} // namespace

TEST_CASE("a registered capability is found by id", "[module][registry]")
{
    resonate::test::FakeHost host;
    ResonateCapabilityRegistry* registry = nullptr;
    REQUIRE(resonate_capability_registry_create(&registry, host.api()) == RESONATE_OK);
    REQUIRE(registry != nullptr);

    FakeCapability instance = {
        RESONATE_CAPABILITY_HEADER_INIT(FakeCapability, RESONATE_THREAD_MAIN), 42};
    const ResonateCapabilityRecord record = recordFor("Resonate.Test.One", 3, instance);
    REQUIRE(resonate_capability_register(registry, &record) == RESONATE_OK);

    std::uint32_t version = 0;
    void* found = resonate_capability_find(registry, record.id, 3, &version);
    REQUIRE(found == &instance);
    REQUIRE(version == 3);

    SECTION("a version above the provider's is refused")
    {
        REQUIRE(resonate_capability_find(registry, record.id, 4, nullptr) == nullptr);
    }

    SECTION("an id nobody registered is not found")
    {
        REQUIRE(resonate_capability_find(registry, resonate_id_make("Resonate.Test.Absent"), 1,
                                         nullptr) == nullptr);
    }

    resonate_capability_registry_destroy(registry);
}

TEST_CASE("two providers for one id are a configuration error", "[module][registry]")
{
    resonate::test::FakeHost host;
    ResonateCapabilityRegistry* registry = nullptr;
    REQUIRE(resonate_capability_registry_create(&registry, host.api()) == RESONATE_OK);

    FakeCapability first = {RESONATE_CAPABILITY_HEADER_INIT(FakeCapability, RESONATE_THREAD_MAIN),
                            1};
    FakeCapability second = {RESONATE_CAPABILITY_HEADER_INIT(FakeCapability, RESONATE_THREAD_MAIN),
                             2};

    const ResonateCapabilityRecord record = recordFor("Resonate.Test.Duplicate", 1, first);
    REQUIRE(resonate_capability_register(registry, &record) == RESONATE_OK);

    const ResonateCapabilityRecord duplicate = recordFor("Resonate.Test.Duplicate", 1, second);
    REQUIRE(resonate_capability_register(registry, &duplicate) == RESONATE_E_INVALID);
    REQUIRE(host.logged("already registered"));

    resonate_capability_registry_destroy(registry);
}

TEST_CASE("only the matching instance is unregistered", "[module][registry]")
{
    resonate::test::FakeHost host;
    ResonateCapabilityRegistry* registry = nullptr;
    REQUIRE(resonate_capability_registry_create(&registry, host.api()) == RESONATE_OK);

    FakeCapability instance = {
        RESONATE_CAPABILITY_HEADER_INIT(FakeCapability, RESONATE_THREAD_MAIN), 7};
    const ResonateCapabilityRecord record = recordFor("Resonate.Test.Unregister", 1, instance);
    REQUIRE(resonate_capability_register(registry, &record) == RESONATE_OK);

    SECTION("a different instance cannot evict the entry")
    {
        FakeCapability other = {
            RESONATE_CAPABILITY_HEADER_INIT(FakeCapability, RESONATE_THREAD_MAIN), 8};
        resonate_capability_unregister(registry, record.id, &other);
        REQUIRE(resonate_capability_find(registry, record.id, 1, nullptr) == &instance);
    }

    SECTION("the matching instance removes the entry")
    {
        resonate_capability_unregister(registry, record.id, &instance);
        REQUIRE(resonate_capability_find(registry, record.id, 1, nullptr) == nullptr);
    }

    resonate_capability_registry_destroy(registry);
}

TEST_CASE("a missing capability is reported before it becomes a null dereference",
          "[module][registry]")
{
    resonate::test::FakeHost host;
    ResonateCapabilityRegistry* registry = nullptr;
    REQUIRE(resonate_capability_registry_create(&registry, host.api()) == RESONATE_OK);

    const ResonateId absent = resonate_id_make("Resonate.Test.Missing");
    REQUIRE(resonate_capability_report_missing(registry, absent, "resonate.physics") == nullptr);
    REQUIRE(host.logged("resonate.physics"));
    REQUIRE(host.logged("not provided"));

    resonate_capability_registry_destroy(registry);
}

#ifdef RESONATE_DEBUG
TEST_CASE("a capability whose header disagrees with its record is rejected", "[module][registry]")
{
    resonate::test::FakeHost host;
    ResonateCapabilityRegistry* registry = nullptr;
    REQUIRE(resonate_capability_registry_create(&registry, host.api()) == RESONATE_OK);

    FakeCapability instance = {
        RESONATE_CAPABILITY_HEADER_INIT(FakeCapability, RESONATE_THREAD_MAIN), 1};
    ResonateCapabilityRecord record = recordFor("Resonate.Test.Mismatch", 1, instance);
    record.struct_size = sizeof(ResonateCapabilityHeader);

    REQUIRE(resonate_capability_register(registry, &record) == RESONATE_E_INVALID);
    REQUIRE(host.logged("its header says"));

    resonate_capability_registry_destroy(registry);
}

TEST_CASE("a capability declaring an unknown thread bit is rejected", "[module][registry]")
{
    resonate::test::FakeHost host;
    ResonateCapabilityRegistry* registry = nullptr;
    REQUIRE(resonate_capability_registry_create(&registry, host.api()) == RESONATE_OK);

    FakeCapability instance = {RESONATE_CAPABILITY_HEADER_INIT(FakeCapability, 1U << 7), 1};
    const ResonateCapabilityRecord record = recordFor("Resonate.Test.Threads", 1, instance);

    REQUIRE(resonate_capability_register(registry, &record) == RESONATE_E_INVALID);
    REQUIRE(host.logged("unknown thread bits"));

    resonate_capability_registry_destroy(registry);
}
#endif

TEST_CASE("registration keeps working past the initial capacity", "[module][registry]")
{
    resonate::test::FakeHost host;
    ResonateCapabilityRegistry* registry = nullptr;
    REQUIRE(resonate_capability_registry_create(&registry, host.api()) == RESONATE_OK);

    constexpr int COUNT = 40;
    FakeCapability instances[COUNT] = {};
    for (int index = 0; index < COUNT; ++index)
    {
        instances[index].header.struct_size = sizeof(FakeCapability);
        instances[index].header.thread_mask = RESONATE_THREAD_MAIN;
        instances[index].value = index;

        const std::string name = "Resonate.Test.Grown." + std::to_string(index);
        const ResonateCapabilityRecord record = recordFor(name.c_str(), 1, instances[index]);
        REQUIRE(resonate_capability_register(registry, &record) == RESONATE_OK);
    }

    for (int index = 0; index < COUNT; ++index)
    {
        const std::string name = "Resonate.Test.Grown." + std::to_string(index);
        INFO("index " << index);
        REQUIRE(resonate_capability_find(registry, resonate_id_make(name.c_str()), 1, nullptr) ==
                &instances[index]);
    }

    resonate_capability_registry_destroy(registry);
}
