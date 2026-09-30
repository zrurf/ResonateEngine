#include <resonate/pal/memory.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <sys/mman.h>

#include "common/memory_accounting.h"
#include "status.h"

/* posix_memalign serves an alignment, not an offset, so the block is
   over-allocated and the caller's pointer is aligned inside it. */
namespace
{

constexpr size_t LARGE_PAGE_OFFSET = 65536;

} // namespace

extern "C"
{

ResonatePalStatus resonate_pal_memory_allocate(void** out_memory, size_t size, size_t alignment)
{
    if (out_memory == nullptr || size == 0U)
    {
        return RESONATE_PAL_INVALID;
    }
    *out_memory = nullptr;

    /* The caller's pointer is aligned by masking, which serves a power of two
       only; anything else is refused here rather than handed back misaligned. */
    if ((alignment & (alignment - 1U)) != 0U)
    {
        return RESONATE_PAL_INVALID;
    }

    const size_t effective = alignment > alignof(resonate::pal::detail::BlockHeader)
                                 ? alignment
                                 : alignof(resonate::pal::detail::BlockHeader);
    const size_t total = sizeof(resonate::pal::detail::BlockHeader) + effective + size;

    void* base = nullptr;
    if (posix_memalign(&base, effective, total) != 0)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    const uintptr_t aligned = (reinterpret_cast<uintptr_t>(base) +
                               sizeof(resonate::pal::detail::BlockHeader) + effective - 1U) &
                              ~(static_cast<uintptr_t>(effective) - 1U);

    auto* header = reinterpret_cast<resonate::pal::detail::BlockHeader*>(aligned) - 1;
    header->base = base;
    header->size = size;
    header->mapped = 0;
    header->large = false;
    resonate::pal::detail::allocations().onAllocate(header);

    *out_memory = reinterpret_cast<void*>(aligned);
    return RESONATE_PAL_OK;
}

void resonate_pal_memory_deallocate(void* memory, size_t size)
{
    if (memory == nullptr)
    {
        return;
    }

    resonate::pal::detail::BlockHeader* header = resonate::pal::detail::headerBefore(memory);
    resonate::pal::detail::allocations().onDeallocate(header);

    if (header->large)
    {
        munmap(header->base, header->mapped);
    }
    else
    {
        free(header->base);
    }

    (void)size;
}

ResonatePalStatus resonate_pal_memory_reallocate(void** out_memory, void* memory, size_t old_size,
                                                 size_t new_size, size_t alignment)
{
    if (out_memory == nullptr || new_size == 0U)
    {
        return RESONATE_PAL_INVALID;
    }

    void* replacement = nullptr;
    const ResonatePalStatus allocated =
        resonate_pal_memory_allocate(&replacement, new_size, alignment);
    if (allocated != RESONATE_PAL_OK)
    {
        return allocated;
    }

    if (memory != nullptr)
    {
        const size_t previous = resonate::pal::detail::headerBefore(memory)->size;
        std::memcpy(replacement, memory, previous < new_size ? previous : new_size);
        resonate_pal_memory_deallocate(memory, old_size);
    }

    *out_memory = replacement;
    return RESONATE_PAL_OK;
}

void resonate_pal_memory_get_stats(ResonateMemoryStats* out_stats)
{
    if (out_stats == nullptr)
    {
        return;
    }
    *out_stats = resonate::pal::detail::allocations().stats();
}

ResonatePalStatus resonate_pal_memory_allocate_large(void** out_memory, size_t size)
{
    if (out_memory == nullptr || size == 0U)
    {
        return RESONATE_PAL_INVALID;
    }
    *out_memory = nullptr;

#if defined(MAP_HUGETLB)
    const size_t total = size + LARGE_PAGE_OFFSET;
    void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (base == MAP_FAILED)
    {
        return RESONATE_PAL_UNSUPPORTED;
    }

    const uintptr_t aligned = reinterpret_cast<uintptr_t>(base) + LARGE_PAGE_OFFSET;
    auto* header = reinterpret_cast<resonate::pal::detail::BlockHeader*>(aligned) - 1;
    header->base = base;
    header->size = size;
    header->mapped = total;
    header->large = true;
    resonate::pal::detail::allocations().onAllocate(header);

    *out_memory = reinterpret_cast<void*>(aligned);
    return RESONATE_PAL_OK;
#else
    return RESONATE_PAL_UNSUPPORTED;
#endif
}

void resonate_pal_memory_free_large(void* memory, size_t size)
{
    resonate_pal_memory_deallocate(memory, size);
}

void resonate_pal_memory_dump_live_allocations(void)
{
#ifdef RESONATE_DEBUG
    resonate::pal::detail::allocations().report();
#endif
}

} // extern "C"
