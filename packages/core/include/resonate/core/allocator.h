#ifndef RESONATE_CORE_ALLOCATOR_H
#define RESONATE_CORE_ALLOCATOR_H

#include <cstddef>

namespace resonate
{

/* Interface for allocations that outlive the code that made them; memory
   crossing the module boundary comes from one shared instance the host reclaims. */
class Allocator
{
  public:
    virtual ~Allocator() = default;

    virtual void* allocate(std::size_t size, std::size_t alignment) = 0;
    virtual void deallocate(void* memory, std::size_t size) = 0;
    virtual void* reallocate(void* memory, std::size_t old_size, std::size_t new_size,
                             std::size_t alignment) = 0;
};

/* Allocates from the platform layer; not callable from a module, which must go
   through the host API. */
Allocator& systemAllocator();

} // namespace resonate

#endif /* RESONATE_CORE_ALLOCATOR_H */
