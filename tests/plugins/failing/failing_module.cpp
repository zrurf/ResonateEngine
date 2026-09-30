/*
 * A plugin that fails its attach after taking host resources: a capability, a
 * signal and an allocation, none of them released. The host has to withdraw all
 * three — a module that fails partway has usually taken something first, and a
 * retry must not find its own ids taken.
 *
 * Written against the C ABI directly: the authoring layer is not what this module
 * exists to exercise, and a plugin may link nothing but the ABI headers.
 */

#include <cstdint>

#include <resonate/module/abi.h>
#include <resonate/module/capability.h>
#include <resonate/module/capability_id.hpp>
#include <resonate/module/message.h>
#include <resonate/module/signal.h>

namespace
{

struct FailingCapability
{
    ResonateCapabilityHeader header =
        RESONATE_CAPABILITY_HEADER_INIT(FailingCapability, RESONATE_THREAD_MAIN);
    void* self = this;
};

constexpr const char* CAPABILITY_NAME = "Resonate.Failing.Capability";

ResonateStatus attach(const ResonateHostApi* host)
{
    if (host == nullptr)
    {
        return RESONATE_E_INTERNAL;
    }

    static FailingCapability capability;

    ResonateCapabilityRecord record = {};
    record.id = resonate::Id(CAPABILITY_NAME).value();
    record.version = 1;
    record.instance = &capability;
    record.struct_size = sizeof(FailingCapability);
    record.name = CAPABILITY_NAME;
    if (host->register_capability(host->user_data, &record) != RESONATE_OK)
    {
        return RESONATE_E_INTERNAL;
    }

    ResonateSignalStorage* signal = nullptr;
    if (host->signal_create(host->user_data, &signal) != RESONATE_OK)
    {
        return RESONATE_E_INTERNAL;
    }

    if (host->allocate(host->user_data, 64U, 8U) == nullptr)
    {
        return RESONATE_E_INTERNAL;
    }

    /* Everything above is left behind on purpose. */
    return RESONATE_E_INTERNAL;
}

void detach()
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
        sizeof(ResonateModuleInfo), "resonate.failing",        "Failing", "0.1.0",
        RESONATE_ABI_VERSION_MAJOR, RESONATE_ABI_VERSION_MAJOR};
    return &info;
}

extern "C" RESONATE_MODULE_EXPORT const ResonateModuleVTable* resonateModuleVTable(void)
{
    static const ResonateModuleVTable table = {sizeof(ResonateModuleVTable), &attach, &detach};
    return &table;
}
