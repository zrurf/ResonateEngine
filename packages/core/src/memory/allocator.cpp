#include <resonate/core/allocator.h>

#include <resonate/pal/memory.h>

namespace resonate
{
namespace
{

/* The engine's own allocator, on top of the platform layer. Memory that crosses
   the module boundary goes through the host, which hands out this instance. */
class SystemAllocator final : public Allocator
{
  public:
    void* allocate(std::size_t size, std::size_t alignment) override
    {
        void* memory = nullptr;
        if (resonate_pal_memory_allocate(&memory, size, alignment) != RESONATE_PAL_OK)
        {
            return nullptr;
        }
        return memory;
    }

    void deallocate(void* memory, std::size_t size) override
    {
        resonate_pal_memory_deallocate(memory, size);
    }

    void* reallocate(void* memory, std::size_t old_size, std::size_t new_size,
                     std::size_t alignment) override
    {
        void* replacement = nullptr;
        if (resonate_pal_memory_reallocate(&replacement, memory, old_size, new_size, alignment) !=
            RESONATE_PAL_OK)
        {
            return nullptr;
        }
        return replacement;
    }
};

} // namespace

Allocator& systemAllocator()
{
    static SystemAllocator allocator;
    return allocator;
}

} // namespace resonate
