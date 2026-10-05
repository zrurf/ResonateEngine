#include <resonate/core/arena.h>

#include <atomic>
#include <cstdint>
#include <new>

namespace resonate
{
namespace
{

constexpr std::size_t kBlockAlignment = alignof(std::max_align_t);

std::byte* alignPointer(std::byte* pointer, std::size_t alignment) noexcept
{
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pointer);
    const std::uintptr_t aligned = (address + alignment - 1U) & ~(alignment - 1U);
    return reinterpret_cast<std::byte*>(aligned);
}

/* One ticket per thread that ever asks for an arena, shared by every
   FrameArenas in the process: the slot index is stable for the thread's
   lifetime and needs no pointer that could dangle with a set. */
std::uint32_t threadTicket() noexcept
{
    static std::atomic<std::uint32_t> next_ticket{0};
    thread_local const std::uint32_t ticket = next_ticket.fetch_add(1U, std::memory_order_relaxed);
    return ticket;
}

/* The set the frame runs on. Written once by the frame's owner before the
   workers read it, and atomic so a later install (the next run in a test
   process) is not a data race. */
std::atomic<FrameArenas*>& activeArenas() noexcept
{
    static std::atomic<FrameArenas*> active{nullptr};
    return active;
}

} // namespace

Arena::Arena(Allocator& allocator, std::size_t block_bytes) noexcept
    : allocator_(&allocator), block_bytes_(block_bytes)
{
}

Arena::~Arena()
{
    while (head_ != nullptr)
    {
        Block* const block = head_;
        head_ = block->next;
        allocator_->deallocate(block, sizeof(Block) + block->size);
    }
}

void* Arena::allocate(std::size_t size, std::size_t alignment)
{
    if (cursor_ != nullptr)
    {
        std::byte* const aligned = alignPointer(cursor_, alignment);
        if (size <= static_cast<std::size_t>(end_ - aligned))
        {
            cursor_ = aligned + size;
            used_ += static_cast<std::size_t>(cursor_ - aligned);
            if (used_ > high_water_)
            {
                high_water_ = used_;
            }
            return aligned;
        }
    }
    return allocateFromNewBlock(size, alignment);
}

void* Arena::allocateFromNewBlock(std::size_t size, std::size_t alignment)
{
    const std::size_t data_alignment = alignment > kBlockAlignment ? alignment : kBlockAlignment;
    const std::size_t block_bytes =
        sizeof(Block) + data_alignment + (size > block_bytes_ ? size : block_bytes_);

    void* memory = allocator_->allocate(block_bytes, kBlockAlignment);
    if (memory == nullptr)
    {
        return nullptr;
    }

    auto* block = static_cast<Block*>(memory);
    block->size = block_bytes - sizeof(Block);
    block->next = head_;
    head_ = block;

    std::byte* const data = alignPointer(block->data(), data_alignment);
    cursor_ = data + size;
    end_ = block->data() + block->size;
    used_ += size;
    if (used_ > high_water_)
    {
        high_water_ = used_;
    }
    return data;
}

void Arena::reset() noexcept
{
    /* Keep the largest block for the next span; its capacity covers the
       common case without touching the allocator again. */
    Block* keep = nullptr;
    std::size_t keep_size = 0;
    Block* block = head_;
    while (block != nullptr)
    {
        Block* const next = block->next;
        if (block->size > keep_size)
        {
            if (keep != nullptr)
            {
                allocator_->deallocate(keep, sizeof(Block) + keep->size);
            }
            keep = block;
            keep_size = block->size;
        }
        else
        {
            allocator_->deallocate(block, sizeof(Block) + block->size);
        }
        block = next;
    }

    if (keep != nullptr)
    {
        keep->next = nullptr;
        head_ = keep;
        cursor_ = alignPointer(keep->data(), kBlockAlignment);
        end_ = keep->data() + keep->size;
    }
    else
    {
        head_ = nullptr;
        cursor_ = nullptr;
        end_ = nullptr;
    }
    used_ = 0;
}

FrameArenas::FrameArenas(Allocator& allocator) noexcept : allocator_(allocator)
{
}

FrameArenas::~FrameArenas() = default;

Arena& FrameArenas::current()
{
    const std::uint32_t slot = threadTicket();

    std::lock_guard<std::mutex> lock(mutex_);
    if (arenas_.size() <= slot)
    {
        arenas_.resize(static_cast<std::size_t>(slot) + 1U);
    }
    if (arenas_[slot] == nullptr)
    {
        arenas_[slot] = std::make_unique<Arena>(allocator_);
    }
    return *arenas_[slot];
}

FrameArenas* FrameArenas::active() noexcept
{
    return activeArenas().load(std::memory_order_acquire);
}

void FrameArenas::setActive(FrameArenas* arenas) noexcept
{
    activeArenas().store(arenas, std::memory_order_release);
}

void FrameArenas::resetAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::unique_ptr<Arena>& arena : arenas_)
    {
        if (arena != nullptr)
        {
            arena->reset();
        }
    }
}

} // namespace resonate
