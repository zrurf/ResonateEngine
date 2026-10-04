#ifndef RESONATE_PAL_TOPOLOGY_H
#define RESONATE_PAL_TOPOLOGY_H

/*
 * The CPU topology of this process: its logical processors, grouped by
 * physical core and by performance class. Probed once and cached for the
 * process's life — the processors a process may use do not change under it —
 * so every caller shares one answer.
 *
 * The probe is a hint, not a contract: a platform that cannot describe its
 * hardware reports a uniform single-class layout, and so does one whose probe
 * fails. Callers work from the reported list alone; nothing distinguishes
 * "uniform hardware" from "unknown layout".
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* A physical core's class, relative to the system's other cores: the primary
   (P) cores are performance class, the secondary (E) ones efficiency class.
   Never unknown — a platform that cannot tell reports performance class, the
   conservative reading. */
typedef enum ResonatePalCoreClass
{
    RESONATE_PAL_CORE_PERFORMANCE = 0,
    RESONATE_PAL_CORE_EFFICIENCY = 1
} ResonatePalCoreClass;

typedef struct ResonatePalCore
{
    /* The processor's placement index: what resonate_pal_thread_create's
       affinity accepts, and what a thread on this processor reports from
       resonate_pal_thread_current_cpu. */
    uint32_t id;

    /* Physical grouping: the logical processors of one physical core share
       (package, core) and carry distinct smt indices from zero. */
    uint32_t package;
    uint32_t core;
    uint32_t smt;

    ResonatePalCoreClass core_class;
} ResonatePalCore;

typedef struct ResonatePalTopology
{
    /* The logical processors, ordered placement-first: performance-class cores
       before efficiency-class ones, physical cores together and ascending — so
       a caller that runs on the first K also runs on the fastest K. */
    const ResonatePalCore* cores;
    uint32_t core_count;

    /* Distinct physical cores, split by class; the two class counts add up to
       physical_cores. */
    uint32_t physical_cores;
    uint32_t performance_cores;
    uint32_t efficiency_cores;
} ResonatePalTopology;

/* Never null and never empty; the first call probes and the result is cached
   for the process's life, so the pointer stays valid. */
const ResonatePalTopology* resonate_pal_topology_query(void);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_TOPOLOGY_H */
