#include <catch2/catch_all.hpp>

#include <resonate/module/module_descriptor.hpp>
#include <resonate/ui/context.h>
#include <resonate/window/window.h>

// Must stay outside the anonymous namespace: the macro opens resonate:: itself.
RESONATE_MODULE_DESCRIPTOR("resonate.test", "Test", "1.2.3", 7,
                           RESONATE_CAPABILITIES(ResonateWindow, ResonateUiContext),
                           RESONATE_CAPABILITIES(ResonateWindow), RESONATE_CAPABILITIES(),
                           RESONATE_MODULE_NAMES("resonate.ui"))

namespace
{

resonate::ModuleManifest makeTestManifest()
{
    return resonate::makeModuleManifest();
}

} // namespace

TEST_CASE("a descriptor reports its id and display name separately", "[module][descriptor]")
{
    const resonate::ModuleManifest manifest = makeTestManifest();

    REQUIRE(manifest.id == "resonate.test");
    REQUIRE(manifest.name == "Test");
    REQUIRE(manifest.version == "1.2.3");
    REQUIRE(manifest.abi == 7);
    REQUIRE(manifest.min_host_abi == 7);
}

TEST_CASE("a descriptor carries capability ids, not names", "[module][descriptor]")
{
    const resonate::ModuleManifest manifest = makeTestManifest();

    REQUIRE(manifest.provides.size() == 2);
    REQUIRE(manifest.requirements.size() == 1);
    REQUIRE(manifest.optional.empty());

    REQUIRE(manifest.provides[0].id == resonate::detail::CapabilityTraits<ResonateWindow>::id);
    REQUIRE(manifest.provides[1].id == resonate::detail::CapabilityTraits<ResonateUiContext>::id);

    /* The name travels with the id, which is what a failed lookup reports. */
    REQUIRE(manifest.provides[0].name ==
            std::string(resonate::detail::CapabilityTraits<ResonateWindow>::name));
}

TEST_CASE("dependencies are module ids and are kept verbatim", "[module][descriptor]")
{
    const resonate::ModuleManifest manifest = makeTestManifest();

    REQUIRE(manifest.depends_on.size() == 1);
    REQUIRE(manifest.depends_on[0] == "resonate.ui");
}
