#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <resonate/core/arena.h>

using resonate::Allocator;
using resonate::Arena;
using resonate::FrameArenas;

namespace
{

/* Records the exact sizes it handed out, so the test can return them the way
   the allocator interface demands. */
class TrackingAllocator final : public Allocator
{
  public:
    void* allocate(std::size_t size, std::size_t alignment) override
    {
        void* const memory = resonate::systemAllocator().allocate(size, alignment);
        if (memory != nullptr)
        {
            live_ += size;
            ++allocations_;
        }
        return memory;
    }

    void deallocate(void* memory, std::size_t size) override
    {
        if (memory == nullptr)
        {
            return;
        }
        live_ -= size;
        resonate::systemAllocator().deallocate(memory, size);
    }

    void* reallocate(void* memory, std::size_t old_size, std::size_t new_size,
                     std::size_t alignment) override
    {
        void* const fresh = allocate(new_size, alignment);
        if (fresh != nullptr && memory != nullptr)
        {
            std::memcpy(fresh, memory, old_size < new_size ? old_size : new_size);
        }
        deallocate(memory, old_size);
        return fresh;
    }

    [[nodiscard]] std::size_t live() const
    {
        return live_;
    }

    [[nodiscard]] std::uint64_t allocations() const
    {
        return allocations_;
    }

  private:
    std::size_t live_ = 0;
    std::uint64_t allocations_ = 0;
};

} // namespace

TEST_CASE("an arena hands out stable, aligned addresses", "[core][arena]")
{
    TrackingAllocator allocator;
    Arena arena(allocator, 1024);

    auto* first = static_cast<std::uint64_t*>(arena.allocate(sizeof(std::uint64_t)));
    REQUIRE(first != nullptr);
    *first = 0x1234;

    auto* aligned = static_cast<std::uint64_t*>(arena.allocate(sizeof(std::uint64_t), 64));
    REQUIRE(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0);

    auto* oversized = arena.allocate(Arena::kBlockBytes * 2U, 1);
    REQUIRE(oversized != nullptr);

    /* Addresses never move, whatever came between. */
    REQUIRE(*first == 0x1234);
    REQUIRE(aligned != reinterpret_cast<std::uint64_t*>(first));
}

TEST_CASE("an arena reset releases its blocks and keeps one for reuse", "[core][arena]")
{
    TrackingAllocator allocator;
    Arena arena(allocator, 4096);

    for (std::uint32_t round = 0; round < 4; ++round)
    {
        /* More than one block per round: the next round must not grow the
           allocator's live set. */
        for (std::uint32_t index = 0; index < 16; ++index)
        {
            REQUIRE(arena.allocate(1024, 1) != nullptr);
        }
        const std::size_t before = arena.usedBytes();
        REQUIRE(before >= 16U * 1024U);

        arena.reset();
        CHECK(arena.usedBytes() == 0);
        CHECK(arena.highWaterBytes() >= before);
    }

    /* One kept block plus whatever the last round added. */
    CHECK(allocator.live() <= Arena::kBlockBytes * 2U + 4096U * 2U);
    CHECK(allocator.allocations() < 64);
}

TEST_CASE("per-thread arenas are distinct and reset together", "[core][arena]")
{
    TrackingAllocator allocator;
    FrameArenas arenas(allocator);

    FrameArenas::setActive(&arenas);

    Arena& main_arena = arenas.current();
    std::mutex seen_mutex;
    std::vector<Arena*> seen;

    std::vector<std::thread> threads;
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        threads.emplace_back(
            [&seen, &seen_mutex]
            {
                Arena& first = FrameArenas::active()->current();
                void* const a = first.allocate(64, 1);
                std::this_thread::yield();

                /* The same thread keeps its arena, and its own bump is
                   contiguous: another thread's allocations cannot sit between
                   two allocations of one thread. */
                Arena& second = FrameArenas::active()->current();
                void* const b = second.allocate(64, 1);
                CHECK(&first == &second);
                CHECK(static_cast<std::byte*>(b) == static_cast<std::byte*>(a) + 64);

                std::lock_guard<std::mutex> lock(seen_mutex);
                seen.push_back(&first);
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    /* Four threads that ran at the same time, four arenas between them. */
    REQUIRE(seen.size() == 4);
    for (std::size_t i = 0; i < seen.size(); ++i)
    {
        for (std::size_t j = i + 1; j < seen.size(); ++j)
        {
            REQUIRE(seen[i] != seen[j]);
        }
    }
    REQUIRE(&arenas.current() == &main_arena);

    arenas.resetAll();
    CHECK(arenas.current().usedBytes() == 0);

    FrameArenas::setActive(nullptr);
}
