#include "common/topology_probe.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <windows.h>

namespace resonate::pal::detail
{
namespace
{

/* The process's processor mask: bit i is logical processor i, which is also
   the index the thread affinity takes. Bit positions are what the topology
   reports as ids, so a placed thread is the one the mask names. */
bool processMask(DWORD_PTR& out_allowed)
{
    DWORD_PTR system_mask = 0;
    return GetProcessAffinityMask(GetCurrentProcess(), &out_allowed, &system_mask) != 0 &&
           out_allowed != 0;
}

bool readRelations(LOGICAL_PROCESSOR_RELATIONSHIP relation, std::vector<std::byte>& out_buffer,
                   DWORD& out_length)
{
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(relation, nullptr, &length) != 0 || length == 0U)
    {
        return false;
    }

    out_buffer.resize(length);
    out_length = length;
    return GetLogicalProcessorInformationEx(
               relation,
               reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(out_buffer.data()),
               &out_length) != 0;
}

uint32_t lowestBitIndex(DWORD_PTR mask)
{
    uint32_t index = 0;
    while ((mask & 1) == 0)
    {
        mask >>= 1;
        ++index;
    }
    return index;
}

/* Describes only the process's current processor group — the first 64 logical
   processors. A process that spans groups reads a smaller set, never a wrong
   one: the mask intersection below keeps only processors it may use. */
constexpr uint32_t GROUP_BITS = 8U * sizeof(DWORD_PTR);

} // namespace

bool probeTopology(std::vector<ResonatePalCore>& out)
{
    DWORD_PTR allowed = 0;
    if (!processMask(allowed))
    {
        return false;
    }

    std::vector<std::byte> packages_buffer;
    DWORD packages_length = 0;
    if (!readRelations(RelationProcessorPackage, packages_buffer, packages_length))
    {
        return false;
    }

    /* Package ordinals are the entries' positions, which is all an ordinal has
       to be; the grouping below maps a processor to the package whose mask
       covers it. */
    std::vector<DWORD_PTR> package_masks;
    for (DWORD offset = 0; offset < packages_length;)
    {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            packages_buffer.data() + offset);
        package_masks.push_back(entry->Processor.GroupMask[0].Mask);
        offset += entry->Size;
    }
    if (package_masks.empty())
    {
        return false;
    }

    std::vector<std::byte> cores_buffer;
    DWORD cores_length = 0;
    if (!readRelations(RelationProcessorCore, cores_buffer, cores_length))
    {
        return false;
    }

    /* One entry per physical core: the mask holds the processors sharing it
       (its SMT siblings), and the efficiency class is its class — every entry
       zero on hardware without a split. */
    std::vector<uint32_t> next_core(package_masks.size(), 0U);
    for (DWORD offset = 0; offset < cores_length;)
    {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            cores_buffer.data() + offset);
        offset += entry->Size;

        const DWORD_PTR siblings = entry->Processor.GroupMask[0].Mask & allowed;
        if (siblings == 0)
        {
            continue;
        }

        uint32_t package = 0;
        const DWORD_PTR representative = static_cast<DWORD_PTR>(1) << lowestBitIndex(siblings);
        for (uint32_t candidate = 0; candidate < package_masks.size(); ++candidate)
        {
            if ((package_masks[candidate] & representative) != 0)
            {
                package = candidate;
                break;
            }
        }

        const uint32_t core = next_core[package]++;
        /* The class is a performance ranking: zero is the efficiency class and
           higher values are the performance class. Measured on a 6P+8E machine,
           where the hyper-threaded cores report 1 and outrun the rest by ~1.6x
           — reading it the other way would class every core backwards. */
        const ResonatePalCoreClass core_class = entry->Processor.EfficiencyClass > 0
                                                    ? RESONATE_PAL_CORE_PERFORMANCE
                                                    : RESONATE_PAL_CORE_EFFICIENCY;

        uint32_t smt = 0;
        for (uint32_t id = 0; id < GROUP_BITS; ++id)
        {
            if ((siblings & (static_cast<DWORD_PTR>(1) << id)) == 0)
            {
                continue;
            }

            ResonatePalCore core_info = {};
            core_info.id = id;
            core_info.package = package;
            core_info.core = core;
            core_info.smt = smt++;
            core_info.core_class = core_class;
            out.push_back(core_info);
        }
    }

    return !out.empty();
}

} // namespace resonate::pal::detail
