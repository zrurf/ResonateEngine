#include <resonate/pal/topology.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <resonate/pal/sync.h>

#include "topology_probe.h"

namespace
{

/* What a platform without a probe gets, and the conservative reading of
   hardware the probe could not describe: one logical processor per hardware
   thread, every one its own physical core of the performance class. */
void buildUniform(std::vector<ResonatePalCore>& cores)
{
    const std::uint32_t count = resonate_pal_sync_hardware_concurrency();
    cores.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index)
    {
        ResonatePalCore core = {};
        core.id = index;
        core.package = 0;
        core.core = index;
        core.smt = 0;
        core.core_class = RESONATE_PAL_CORE_PERFORMANCE;
        cores.push_back(core);
    }
}

bool placementLess(const ResonatePalCore& a, const ResonatePalCore& b)
{
    if (a.core_class != b.core_class)
    {
        return a.core_class < b.core_class;
    }
    if (a.package != b.package)
    {
        return a.package < b.package;
    }
    if (a.core != b.core)
    {
        return a.core < b.core;
    }
    return a.smt < b.smt;
}

std::uint32_t countPhysicalCores(const std::vector<ResonatePalCore>& cores,
                                 ResonatePalCoreClass core_class)
{
    std::uint32_t count = 0;
    for (std::size_t index = 0; index < cores.size(); ++index)
    {
        const ResonatePalCore& core = cores[index];
        if (core.core_class != core_class)
        {
            continue;
        }

        bool first = true;
        for (std::size_t earlier = 0; earlier < index && first; ++earlier)
        {
            first = cores[earlier].package != core.package || cores[earlier].core != core.core;
        }
        if (first)
        {
            ++count;
        }
    }
    return count;
}

/* Probed once, through a function-local static, so the guard variable is the
   thread-safe lazy initialisation and the pointers stay valid for the life of
   the process — which is what lets the query hand out a borrowed pointer. */
struct Cache
{
    std::vector<ResonatePalCore> cores;
    ResonatePalTopology topology = {};

    Cache()
    {
        if (!resonate::pal::detail::probeTopology(cores) || cores.empty())
        {
            cores.clear();
            buildUniform(cores);
        }

        std::stable_sort(cores.begin(), cores.end(), &placementLess);

        topology.cores = cores.data();
        topology.core_count = static_cast<std::uint32_t>(cores.size());
        topology.performance_cores = countPhysicalCores(cores, RESONATE_PAL_CORE_PERFORMANCE);
        topology.efficiency_cores = countPhysicalCores(cores, RESONATE_PAL_CORE_EFFICIENCY);
        topology.physical_cores = topology.performance_cores + topology.efficiency_cores;
    }
};

} // namespace

extern "C"
{

const ResonatePalTopology* resonate_pal_topology_query(void)
{
    static const Cache cache;
    return &cache.topology;
}

} // extern "C"
