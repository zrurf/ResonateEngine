#include <catch2/catch_all.hpp>

#include <string>

#include <resonate/core/allocator.h>
#include <resonate/module/host.hpp>
#include <resonate/pal/memory.h>
#include <resonate/render/offscreen.hpp>

#include "paths.h"

namespace
{

ResonateMemoryStats liveBlocks()
{
    ResonateMemoryStats stats = {};
    resonate_pal_memory_get_stats(&stats);
    return stats;
}

} // namespace

/*
 * The ABI promises the host reclaims host memory a module does not give back
 * ("Reclaimed by the host; a module must not free host memory itself"), and every
 * module allocates its state that way. A module that leaves it behind must not
 * leave the host holding it.
 */
TEST_CASE("a module that leaves its state behind does not leak it", "[module][loader][memory]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    INFO("the plugin targets are a build dependency of this test target");
    REQUIRE_FALSE(plugin_directory.empty());

    /* Taken before the host exists: the host's own storage is freed with it, so
       what is measured is what the modules took and gave back. */
    const ResonateMemoryStats before = liveBlocks();

    {
        auto host = resonate::ModuleHost::create(resonate::systemAllocator());
        REQUIRE(host != nullptr);

        /* What the modules that draw require; which canvas it is does not matter
           here, only that the capability resolves. */
        REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 640U, 480U) ==
                RESONATE_OK);

        REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
        REQUIRE(host->resolve() == RESONATE_OK);
        REQUIRE(host->attachAll() == RESONATE_OK);
        host->detachAll();
    }

    const ResonateMemoryStats after = liveBlocks();
    REQUIRE(after.live_blocks == before.live_blocks);
    REQUIRE(after.allocated_bytes == before.allocated_bytes);
}
