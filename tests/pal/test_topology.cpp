#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <resonate/pal/sync.h>
#include <resonate/pal/thread.h>
#include <resonate/pal/topology.h>

/*
 * The topology probe: what the job pool sizes itself by and places its workers
 * on. The invariants hold on any machine; a case that needs a particular layout
 * checks the machine first.
 */

namespace
{

using CoreKey = std::pair<std::uint32_t, std::uint32_t>; /* package, core */

/* Runs with the affinity of the topology's last processor — the least
   performance-class one on a split machine — and reports where it landed. */
struct Placement
{
    std::uint32_t expected = 0;
    std::uint32_t landed = RESONATE_PAL_THREAD_CPU_UNKNOWN;
    std::uint32_t after_priority = RESONATE_PAL_THREAD_CPU_UNKNOWN;

    static void run(void* argument)
    {
        auto* placement = static_cast<Placement*>(argument);

        /* Placement was made before this runs; the kernel may still be
           migrating the thread, so give it a moment to land. */
        for (int spin = 0; spin < 100000; ++spin)
        {
            if (resonate_pal_thread_current_cpu() == placement->expected)
            {
                break;
            }
            resonate_pal_thread_yield();
        }
        placement->landed = resonate_pal_thread_current_cpu();

        /* A priority is a hint, not a placement: asking for another level must
           leave the thread where it is. */
        resonate_pal_thread_set_priority(RESONATE_PAL_THREAD_PRIORITY_LOW);
        placement->after_priority = resonate_pal_thread_current_cpu();
    }
};

} // namespace

TEST_CASE("the topology probe describes this process's processors", "[pal][topology]")
{
    const ResonatePalTopology* topology = resonate_pal_topology_query();
    REQUIRE(topology != nullptr);
    REQUIRE(topology->cores != nullptr);
    REQUIRE(topology->core_count >= 1U);

    /* Never more than the machine offers, and cached: probing cannot change
       while the process lives, so every caller gets the same answer. */
    REQUIRE(topology->core_count <= resonate_pal_sync_hardware_concurrency());
    REQUIRE(resonate_pal_topology_query() == topology);

    std::set<std::uint32_t> ids;
    std::map<CoreKey, int> class_per_core;
    std::map<CoreKey, std::vector<std::uint32_t>> smt_per_core;

    for (std::uint32_t index = 0; index < topology->core_count; ++index)
    {
        const ResonatePalCore& core = topology->cores[index];

        /* Placement ids address distinct processors. */
        REQUIRE(ids.insert(core.id).second);

        /* Placement order: performance-class processors before efficiency-class
           ones, so a caller that takes the first K gets the fastest K. */
        if (index > 0U)
        {
            REQUIRE(topology->cores[index - 1U].core_class <= core.core_class);
        }

        /* One class per physical core, and only the two that exist. */
        REQUIRE((core.core_class == RESONATE_PAL_CORE_PERFORMANCE ||
                 core.core_class == RESONATE_PAL_CORE_EFFICIENCY));
        const CoreKey key{core.package, core.core};
        const auto inserted = class_per_core.emplace(key, static_cast<int>(core.core_class));
        REQUIRE((inserted.second || inserted.first->second == static_cast<int>(core.core_class)));
        smt_per_core[key].push_back(core.smt);
    }

    /* The SMT threads of one core are numbered 0..n-1 and adjacent: the order
       groups them. */
    for (const auto& [key, smts] : smt_per_core)
    {
        for (std::size_t index = 0; index < smts.size(); ++index)
        {
            REQUIRE(smts[index] == index);
        }
    }

    /* The class counts are derived from the same list. */
    std::uint32_t performance = 0;
    std::uint32_t efficiency = 0;
    for (const auto& [key, core_class] : class_per_core)
    {
        if (core_class == static_cast<int>(RESONATE_PAL_CORE_PERFORMANCE))
        {
            ++performance;
        }
        else
        {
            ++efficiency;
        }
    }
    REQUIRE(topology->physical_cores == class_per_core.size());
    REQUIRE(topology->performance_cores == performance);
    REQUIRE(topology->efficiency_cores == efficiency);
}

TEST_CASE("a placed thread runs on the processor it asked for", "[pal][topology][thread]")
{
    const ResonatePalTopology* topology = resonate_pal_topology_query();
    const std::uint32_t target = topology->cores[topology->core_count - 1U].id;

    Placement placement;
    placement.expected = target;

    ResonateThreadHandle thread = {};
    REQUIRE(resonate_pal_thread_create(&thread, &Placement::run, &placement, 0,
                                       static_cast<int32_t>(target)) == RESONATE_PAL_OK);
    REQUIRE(resonate_pal_thread_join(&thread) == RESONATE_PAL_OK);
    resonate_pal_thread_destroy(&thread);

    REQUIRE(placement.landed == target);
    REQUIRE(placement.after_priority == target);
}
