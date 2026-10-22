#include <resonate/csharp/dispatch.h>

namespace resonate::csharp
{

DispatchTable::DispatchTable(std::size_t capacity)
{
    resize(capacity);
}

void DispatchTable::resize(std::size_t capacity)
{
    /* An empty table still owns one slot: the array is never null, so lookups
       stay branch-free apart from the range check. */
    entries_ = std::make_unique<std::atomic<DispatchEntry>[]>(capacity == 0U ? 1U : capacity);
    for (std::size_t index = 0; index < capacity; ++index)
    {
        entries_[index].store(nullptr, std::memory_order_relaxed);
    }
    capacity_ = capacity;
}

bool DispatchTable::set(std::uint32_t id, DispatchEntry entry)
{
    if (id >= capacity_ || entry == nullptr)
    {
        return false;
    }
    DispatchEntry expected = nullptr;
    return entries_[id].compare_exchange_strong(expected, entry, std::memory_order_release);
}

bool DispatchTable::replace(std::uint32_t id, DispatchEntry entry)
{
    if (id >= capacity_ || entry == nullptr)
    {
        return false;
    }
    entries_[id].store(entry, std::memory_order_release);
    return true;
}

bool DispatchTable::clear(std::uint32_t id)
{
    if (id >= capacity_)
    {
        return false;
    }
    entries_[id].store(nullptr, std::memory_order_release);
    return true;
}

DispatchEntry DispatchTable::get(std::uint32_t id) const
{
    if (id >= capacity_)
    {
        return nullptr;
    }
    return entries_[id].load(std::memory_order_acquire);
}

bool DispatchTable::invoke(std::uint32_t id, void* context) const
{
    const DispatchEntry entry = get(id);
    if (entry == nullptr)
    {
        return false;
    }
    entry(context);
    return true;
}

bool DispatchTable::any() const
{
    for (std::size_t index = 0; index < capacity_; ++index)
    {
        if (entries_[index].load(std::memory_order_acquire) != nullptr)
        {
            return true;
        }
    }
    return false;
}

} // namespace resonate::csharp
