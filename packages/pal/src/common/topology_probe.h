#ifndef RESONATE_PAL_COMMON_TOPOLOGY_PROBE_H
#define RESONATE_PAL_COMMON_TOPOLOGY_PROBE_H

#include <vector>

#include <resonate/pal/topology.h>

namespace resonate::pal::detail
{

/* The platform's part of the topology probe: appends the logical processors
   this process may run on, with their grouping and class, in the platform's
   order. False when the platform has no probe or could not describe the
   layout; the caller then reports a uniform single-class one instead. */
bool probeTopology(std::vector<ResonatePalCore>& out);

} // namespace resonate::pal::detail

#endif /* RESONATE_PAL_COMMON_TOPOLOGY_PROBE_H */
