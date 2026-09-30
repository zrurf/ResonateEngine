#include "resonate/module/host.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace resonate
{
namespace
{

/* The graph indexes capabilities by hash, and a 128-bit id is awkward as a map
   key on its own. */
std::uint64_t keyOf(ResonateId id)
{
    return id.lo ^ (id.hi * 0x9E3779B97F4A7C15ULL);
}

} // namespace

ResonateStatus ModuleGraph::build(std::vector<ModuleRecord>& records)
{
    order_.clear();
    error_.clear();

    const std::size_t count = records.size();

    std::unordered_map<std::string, std::size_t> index_of;
    for (std::size_t index = 0; index < count; ++index)
    {
        index_of.emplace(records[index].manifest.id, index);
    }

    std::unordered_map<std::uint64_t, std::size_t> provider_of;
    for (std::size_t index = 0; index < count; ++index)
    {
        for (const ModuleCapability& provided : records[index].manifest.provides)
        {
            provider_of[keyOf(provided.id.value())] = index;
        }
    }

    /* prerequisites[i] holds the modules that must attach before i. An explicit
       depends_on and a required capability can name the same provider, so the
       lists are deduplicated before they become graph edges. */
    std::vector<std::vector<std::size_t>> prerequisites(count);

    for (std::size_t index = 0; index < count; ++index)
    {
        const ModuleManifest& manifest = records[index].manifest;

        for (const std::string& dependency : manifest.depends_on)
        {
            const auto found = index_of.find(dependency);
            if (found == index_of.end())
            {
                error_ = manifest.id + " depends on '" + dependency + "', which is not present";
                return RESONATE_E_MISSING;
            }
            if (found->second != index)
            {
                prerequisites[index].push_back(found->second);
            }
        }

        for (const ModuleCapability& requirement : manifest.requirements)
        {
            const auto found = provider_of.find(keyOf(requirement.id.value()));
            if (found != provider_of.end() && found->second != index)
            {
                prerequisites[index].push_back(found->second);
            }
        }

        std::vector<std::size_t>& list = prerequisites[index];
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }

    std::vector<std::size_t> remaining(count, 0);
    for (std::size_t index = 0; index < count; ++index)
    {
        remaining[index] = prerequisites[index].size();
    }

    /* Lowest index first, so the order is the same on every run. */
    std::vector<bool> emitted(count, false);
    while (true)
    {
        std::size_t next = count;
        for (std::size_t index = 0; index < count; ++index)
        {
            if (!emitted[index] && remaining[index] == 0)
            {
                next = index;
                break;
            }
        }
        if (next == count)
        {
            break;
        }

        emitted[next] = true;
        order_.push_back(&records[next]);

        for (std::size_t index = 0; index < count; ++index)
        {
            if (!emitted[index] &&
                std::find(prerequisites[index].begin(), prerequisites[index].end(), next) !=
                    prerequisites[index].end())
            {
                --remaining[index];
            }
        }
    }

    if (order_.size() != count)
    {
        /* Walk the modules that could not be emitted until one repeats: what is
           between the two visits is the cycle. */
        std::string path;
        std::size_t start = count;
        for (std::size_t index = 0; index < count; ++index)
        {
            if (!emitted[index] && !prerequisites[index].empty())
            {
                start = index;
                break;
            }
        }

        if (start != count)
        {
            /* Only an un-emitted prerequisite can lead further into the cycle: a
               module whose prerequisites were all emitted would have been emitted
               itself. Following the first one therefore keeps the walk inside the
               un-emitted set and ends at a repeat. */
            std::vector<std::size_t> visited;
            std::size_t current = start;
            while (std::find(visited.begin(), visited.end(), current) == visited.end())
            {
                visited.push_back(current);
                if (!path.empty())
                {
                    path += " -> ";
                }
                path += records[current].manifest.id;

                for (const std::size_t candidate : prerequisites[current])
                {
                    if (!emitted[candidate])
                    {
                        current = candidate;
                        break;
                    }
                }
            }
            path += " -> " + records[current].manifest.id;
        }

        error_ = "the dependency graph has a cycle: " +
                 (path.empty() ? std::string("(unresolved)") : path);
        order_.clear();
        return RESONATE_E_INVALID;
    }

    return RESONATE_OK;
}

} // namespace resonate
