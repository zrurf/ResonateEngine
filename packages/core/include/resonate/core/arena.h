#ifndef RESONATE_CORE_ARENA_H
#define RESONATE_CORE_ARENA_H

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "resonate/core/allocator.h"

namespace resonate
{

/* Bump allocator for frame-scoped scratch. Allocation advances a cursor and
   reset() frees everything at once, which is what makes per-frame data —
   intent queues, scheduling metadata — allocation-free after the first frame.
   One owner at a time; the per-thread instances live in FrameArenas. */
class Arena
{
  public:
    /* A request that does not fit the free tail starts a block of at least
       this many bytes, or more when the request itself is larger. */
    static constexpr std::size_t kBlockBytes = 64U * 1024U;

    explicit Arena(Allocator& allocator, std::size_t block_bytes = kBlockBytes) noexcept;
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    /* Never null; the address is stable until reset. */
    void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

    /* Default-constructs one T from the bump. */
    template <typename T> T* allocate()
    {
        return static_cast<T*>(allocate(sizeof(T), alignof(T)));
    }

    /* Releases every block but the largest, which is kept for the next span. */
    void reset() noexcept;

    [[nodiscard]] std::size_t blockBytes() const noexcept
    {
        return block_bytes_;
    }

    /* Handed out and not yet reset. */
    [[nodiscard]] std::size_t usedBytes() const noexcept
    {
        return used_;
    }

    /* The usedBytes maximum since construction. */
    [[nodiscard]] std::size_t highWaterBytes() const noexcept
    {
        return high_water_;
    }

  private:
    /* Header of one allocation from the backing allocator; the bumpable bytes
       follow it. */
    struct Block
    {
        Block* next = nullptr;
        std::size_t size = 0;

        [[nodiscard]] std::byte* data() noexcept
        {
            return reinterpret_cast<std::byte*>(this) + sizeof(Block);
        }
    };

    void* allocateFromNewBlock(std::size_t size, std::size_t alignment);

    Allocator* allocator_ = nullptr;
    std::size_t block_bytes_ = 0;
    Block* head_ = nullptr; /* every block ever taken, newest first */
    std::byte* cursor_ = nullptr;
    std::byte* end_ = nullptr;
    std::size_t used_ = 0;
    std::size_t high_water_ = 0;
};

/* One arena per thread over one allocator: a job body on the pool reaches its
   own scratch through current(), and resetAll() reclaims every block at the
   frame's boundary, when nothing that points into an arena is alive.

   The set the frame runs on is installed once (setActive) and read from any
   thread; code outside a frame reads null and allocates its own way. */
class FrameArenas
{
  public:
    explicit FrameArenas(Allocator& allocator) noexcept;
    ~FrameArenas();

    FrameArenas(const FrameArenas&) = delete;
    FrameArenas& operator=(const FrameArenas&) = delete;

    /* This thread's arena, created on first use and kept for the life of the
       set: a worker's next frame reuses its blocks. Slots are numbered by a
       process-wide ticket, so a slot an exited thread leaves behind costs one
       null pointer and no arena until somebody reuses the ticket. */
    [[nodiscard]] Arena& current();

    [[nodiscard]] static FrameArenas* active() noexcept;
    static void setActive(FrameArenas* arenas) noexcept;

    void resetAll();

  private:
    Allocator& allocator_;

    /* Guarded by mutex_; indexed by thread ticket, null until that thread
       first asks. */
    std::mutex mutex_;
    std::vector<std::unique_ptr<Arena>> arenas_;
};

} // namespace resonate

#endif /* RESONATE_CORE_ARENA_H */
