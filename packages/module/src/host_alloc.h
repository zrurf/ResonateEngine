#ifndef RESONATE_MODULE_SRC_HOST_ALLOC_H
#define RESONATE_MODULE_SRC_HOST_ALLOC_H

#include <stddef.h>

#include "resonate/module/abi.h"

namespace resonate::detail
{

/* The host allocator is the only source of memory this package may use. */
inline void* hostAllocate(const ResonateHostApi* host, size_t size, size_t alignment)
{
    return host != nullptr ? host->allocate(host->user_data, size, alignment) : nullptr;
}

inline void hostDeallocate(const ResonateHostApi* host, void* memory, size_t size)
{
    if (host != nullptr && memory != nullptr)
    {
        host->deallocate(host->user_data, memory, size);
    }
}

inline void hostLog(const ResonateHostApi* host, int32_t level, const char* message)
{
    if (host != nullptr && host->log != nullptr)
    {
        host->log(host->user_data, level, __FILE__, __LINE__, message);
    }
}

} // namespace resonate::detail

#endif /* RESONATE_MODULE_SRC_HOST_ALLOC_H */
