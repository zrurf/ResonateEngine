#ifndef RESONATE_PAL_SRC_COMMON_MEMORY_ACCOUNTING_H
#define RESONATE_PAL_SRC_COMMON_MEMORY_ACCOUNTING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include <resonate/pal/memory.h>

namespace resonate::pal::detail
{

/* Sits immediately before every block the memory backends hand out, so a block
   can be freed, accounted for and enumerated from the returned pointer alone. */
struct BlockHeader
{
    void* base;
    std::size_t size;
    std::size_t mapped;
    BlockHeader* previous;
    BlockHeader* next;
    bool large;
};

inline BlockHeader* headerBefore(void* memory)
{
    return reinterpret_cast<BlockHeader*>(static_cast<unsigned char*>(memory) -
                                          sizeof(BlockHeader));
}

/* Shared by both backends: one set of counters and one live list, so the numbers
   mean the same thing everywhere. */
class Allocations
{
  public:
    void onAllocate(BlockHeader* header)
    {
        header->previous = nullptr;
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            header->next = head_;
            if (head_ != nullptr)
            {
                head_->previous = header;
            }
            head_ = header;
        }

        const std::uint64_t allocated = allocated_bytes_.fetch_add(header->size) + header->size;
        live_blocks_.fetch_add(1);

        std::uint64_t peak = peak_bytes_.load();
        while (peak < allocated && !peak_bytes_.compare_exchange_weak(peak, allocated))
        {
        }
    }

    void onDeallocate(BlockHeader* header)
    {
        {
            const std::lock_guard<std::mutex> guard(mutex_);
            if (header->previous != nullptr)
            {
                header->previous->next = header->next;
            }
            else
            {
                head_ = header->next;
            }
            if (header->next != nullptr)
            {
                header->next->previous = header->previous;
            }
        }

        allocated_bytes_.fetch_sub(header->size);
        live_blocks_.fetch_sub(1);
    }

    [[nodiscard]] ResonateMemoryStats stats() const
    {
        ResonateMemoryStats result = {};
        result.allocated_bytes = allocated_bytes_.load();
        result.peak_bytes = peak_bytes_.load();
        result.live_blocks = live_blocks_.load();
        return result;
    }

    void report() const
    {
        const ResonateMemoryStats current = stats();
        std::fprintf(stderr, "pal memory: %llu byte(s) live in %llu block(s), peak %llu\n",
                     static_cast<unsigned long long>(current.allocated_bytes),
                     static_cast<unsigned long long>(current.live_blocks),
                     static_cast<unsigned long long>(current.peak_bytes));

        const std::lock_guard<std::mutex> guard(mutex_);
        for (const BlockHeader* header = head_; header != nullptr; header = header->next)
        {
            std::fprintf(stderr, "  %p  %zu byte(s)%s\n", static_cast<const void*>(header + 1),
                         header->size, header->large ? "  (large page)" : "");
        }
    }

  private:
    mutable std::mutex mutex_;
    BlockHeader* head_ = nullptr;
    std::atomic<std::uint64_t> allocated_bytes_{0};
    std::atomic<std::uint64_t> peak_bytes_{0};
    std::atomic<std::uint64_t> live_blocks_{0};
};

inline Allocations& allocations()
{
    static Allocations instance;
    return instance;
}

} // namespace resonate::pal::detail

#endif /* RESONATE_PAL_SRC_COMMON_MEMORY_ACCOUNTING_H */
