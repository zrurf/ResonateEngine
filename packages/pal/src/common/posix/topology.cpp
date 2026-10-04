#include "common/topology_probe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(__linux__)
#    include <sched.h>
#endif

namespace resonate::pal::detail
{
namespace
{

#if defined(__linux__)

/* One number from a sysfs file; false when the file is absent, which is how a
   kernel that does not publish the attribute answers. */
bool readNumber(const char* format, int cpu, long& out_value)
{
    char path[256] = {};
    std::snprintf(path, sizeof(path), format, cpu);

    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }

    long value = 0;
    const int scanned = std::fscanf(file, "%ld", &value);
    std::fclose(file);
    if (scanned != 1)
    {
        return false;
    }

    out_value = value;
    return true;
}

/* Speeds in the kernel's own scale, which only comparisons between this
   system's processors use. Read for every processor or for none: cpu_capacity
   and a frequency are different units, and one missing attribute would
   misplace a single core's class. */
void readSpeeds(const std::vector<int>& cpus, std::vector<long>& out_speeds)
{
    for (const char* format : {"/sys/devices/system/cpu/cpu%d/cpu_capacity",
                               "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq"})
    {
        out_speeds.clear();
        out_speeds.reserve(cpus.size());

        bool complete = true;
        for (const int cpu : cpus)
        {
            long value = 0;
            if (!readNumber(format, cpu, value))
            {
                complete = false;
                break;
            }
            out_speeds.push_back(value);
        }
        if (complete)
        {
            return;
        }
    }
    out_speeds.clear();
}

void assignSmtIndices(std::vector<ResonatePalCore>& cores)
{
    for (std::size_t index = 0; index < cores.size(); ++index)
    {
        uint32_t smt = 0;
        for (std::size_t earlier = 0; earlier < index; ++earlier)
        {
            if (cores[earlier].package == cores[index].package &&
                cores[earlier].core == cores[index].core)
            {
                ++smt;
            }
        }
        cores[index].smt = smt;
    }
}

bool probeLinux(std::vector<ResonatePalCore>& out)
{
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    {
        return false;
    }

    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (CPU_ISSET(cpu, &allowed) != 0)
        {
            cpus.push_back(cpu);
        }
    }
    if (cpus.empty())
    {
        return false;
    }

    std::vector<long> speeds;
    readSpeeds(cpus, speeds);
    const long fastest = speeds.empty() ? 0 : *std::max_element(speeds.begin(), speeds.end());

    /* The core grouping comes from sysfs; a kernel that does not publish it
       (some virtual machines) still contributes the processor set, which is
       what pool sizing needs. */
    std::vector<uint32_t> core_ids(cpus.size(), 0U);
    std::vector<uint32_t> package_ids(cpus.size(), 0U);
    bool described = true;
    for (std::size_t index = 0; index < cpus.size() && described; ++index)
    {
        long core_id = 0;
        long package_id = 0;
        described =
            readNumber("/sys/devices/system/cpu/cpu%d/topology/core_id", cpus[index], core_id) &&
            readNumber("/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpus[index],
                       package_id);
        if (described)
        {
            core_ids[index] = static_cast<uint32_t>(core_id);
            package_ids[index] = static_cast<uint32_t>(package_id);
        }
    }

    for (std::size_t index = 0; index < cpus.size(); ++index)
    {
        ResonatePalCore core = {};
        core.id = static_cast<uint32_t>(cpus[index]);
        core.package = package_ids[index];
        core.core = described ? core_ids[index] : static_cast<uint32_t>(index);
        core.smt = 0;
        /* A processor slower than the fastest one is efficiency class; equal
           reads as performance, so a system without a split is uniformly
           performance. */
        core.core_class = (described && !speeds.empty() && speeds[index] < fastest)
                              ? RESONATE_PAL_CORE_EFFICIENCY
                              : RESONATE_PAL_CORE_PERFORMANCE;
        out.push_back(core);
    }

    assignSmtIndices(out);
    return true;
}

#endif /* __linux__ */

} // namespace

bool probeTopology(std::vector<ResonatePalCore>& out)
{
#if defined(__linux__)
    return probeLinux(out);
#else
    /* Apple publishes no P/E query; its QoS classes are the placement hint
       instead, which the thread priority carries. */
    return false;
#endif
}

} // namespace resonate::pal::detail
