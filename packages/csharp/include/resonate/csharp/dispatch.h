#ifndef RESONATE_CSHARP_DISPATCH_H
#define RESONATE_CSHARP_DISPATCH_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace resonate::csharp
{

/* A registered system's frame entry. The table holds these instead of any
   managed delegate: the engine never owns a pointer it would have to update,
   and a patch swaps the slot atomically. */
using DispatchEntry = void (*)(void* context);

/* Stable id -> entry. Ids are generated per registered system and do not move
   when a patch replaces the implementation. */
class DispatchTable
{
public:
    explicit DispatchTable(std::size_t capacity = 0);

    /* Allocates the slots, all empty. Called by the assembler before any
       managed code registers; entries do not survive a resize. */
    void resize(std::size_t capacity);

    /* Registers an entry once. Refuses an occupied slot, a null entry and an
       out-of-range id. */
    bool set(std::uint32_t id, DispatchEntry entry);

    /* The patch path: replaces an entry unconditionally. A concurrent caller
       observes either the old or the new entry. */
    bool replace(std::uint32_t id, DispatchEntry entry);

    DispatchEntry get(std::uint32_t id) const;

    /* Runs the slot when one is registered and reports whether it ran; an
       empty slot is a no-op, so a system a patch has not installed yet cannot
       crash the frame. */
    bool invoke(std::uint32_t id, void* context) const;

    std::size_t capacity() const
    {
        return capacity_;
    }

private:
    /* An array of atomics is not movable; the unique_ptr keeps the table
       movable while each slot stays independently atomic. */
    std::unique_ptr<std::atomic<DispatchEntry>[]> entries_;
    std::size_t capacity_ = 0;
};

} // namespace resonate::csharp

#endif /* RESONATE_CSHARP_DISPATCH_H */
