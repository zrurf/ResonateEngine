/*
 * A plugin that answers RESONATE_E_UNSUPPORTED at attach, before taking
 * anything: the shape of a render backend the configuration did not choose.
 * The run continues without it — the loader skips the record rather than
 * failing the attach pass, which is what the loader test pins.
 *
 * Written against the C ABI directly, like its failing sibling.
 */

#include <resonate/module/abi.h>

namespace
{

ResonateStatus attach(const ResonateHostApi* host)
{
    (void)host;
    return RESONATE_E_UNSUPPORTED;
}

void detach(void)
{
}

} // namespace

extern "C" RESONATE_MODULE_EXPORT const ResonateModuleInfo* resonateModuleInfo(uint32_t host_abi)
{
    if ((host_abi >> 16) != RESONATE_ABI_VERSION_MAJOR)
    {
        return nullptr;
    }

    static const ResonateModuleInfo info = {
        sizeof(ResonateModuleInfo), "resonate.skipped",        "Skipped", "0.1.0",
        RESONATE_ABI_VERSION_MAJOR, RESONATE_ABI_VERSION_MAJOR};
    return &info;
}

extern "C" RESONATE_MODULE_EXPORT const ResonateModuleVTable* resonateModuleVTable(void)
{
    static const ResonateModuleVTable table = {sizeof(ResonateModuleVTable), &attach, &detach};
    return &table;
}
