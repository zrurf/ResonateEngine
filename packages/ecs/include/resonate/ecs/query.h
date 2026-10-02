#ifndef RESONATE_ECS_QUERY_H
#define RESONATE_ECS_QUERY_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "resonate/core/job.h"
#include "resonate/core/span.h"
#include "resonate/ecs/component.h"
#include "resonate/ecs/entity.h"

namespace resonate::ecs
{

class World;

namespace detail
{
struct WorldImpl;
struct Archetype;
struct Chunk;
} // namespace detail

/* What a query selects: an archetype matches when it has every component in
   `all`, at least one of `any` (an empty `any` poses no constraint), and none of
   `none`. The spans borrow the caller's arrays; the query keeps only the masks
   it builds from them. */
struct QueryDesc
{
    Span<const ComponentIndex> all;
    Span<const ComponentIndex> any;
    Span<const ComponentIndex> none;
};

/* One chunk's dense contents, as an iteration body sees it. Pointers live for
   the duration of the body only: a structural change invalidates them, and one
   during iteration is refused — law 2's command buffer is the path. Concurrent
   bodies (parallel iteration) each have their own chunk; a body must not touch
   another chunk's arrays. */
struct ChunkView
{
    /* One component of the chunk's archetype. */
    struct Column
    {
        ComponentIndex component = kInvalidComponent;
        void* data = nullptr;
        std::uint32_t* ticks = nullptr;
    };

    std::uint32_t count = 0;
    const Entity* entities = nullptr;

    /* One per component of the archetype, ascending by component index. */
    const Column* columns = nullptr;
    std::uint32_t columnCount = 0;

    /* The chunk this view was made from and the structural change count at that
       moment. A view is only usable until something structural happens to the
       chunk; `valid()` reports whether one did (law 6). */
    const void* chunk = nullptr;
    std::uint32_t version = 0;

    [[nodiscard]] bool valid() const noexcept;

    /* The column's data or tick array, or null when the chunk does not have the
       component. A body that scans densely takes the column once, outside its
       loop, rather than looking it up per entity. */
    [[nodiscard]] void* componentData(ComponentIndex component) const noexcept;
    [[nodiscard]] std::uint32_t* componentTicks(ComponentIndex component) const noexcept;
};

/* Called once per chunk, on the iterating thread. */
using ChunkFunction = void (*)(void* context, ChunkView view);

/* A pre-registered query group. The archetypes it matches are cached and grown
   as new archetypes appear, so iteration never scans the ones it has already
   judged. A query belongs to the world that created it, is not thread-safe (one
   thread iterates it at a time) and is not copyable; moving is fine. */
class Query
{
  public:
    Query() = default;
    ~Query() = default;
    Query(Query&& other) noexcept = default;
    Query& operator=(Query&& other) noexcept = default;

    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;

    /* False for a default-constructed query and for one whose description named
       a component the world did not register. */
    [[nodiscard]] bool valid() const noexcept
    {
        return impl_ != nullptr && valid_;
    }

    /* Archetypes matched so far, after catching up with the archetypes that
       appeared since the last call. */
    [[nodiscard]] std::uint32_t matchedArchetypeCount();

    /* Chunks that would be visited, counting only non-empty ones. */
    [[nodiscard]] std::uint32_t matchedChunkCount();

    /* Visits every matched, non-empty chunk: archetypes in creation order,
       chunks in order, rows in order. */
    void forEachChunk(ChunkFunction body, void* context);

    /* The same visit, sharded across the job pool: one slice of chunks per job
       entry, `reads`/`writes` being the caller's declaration of what the body
       touches, so the pool serialises it against other work on those groups.
       Returns when every chunk has been visited. */
    void parallelEachChunk(JobSystem& jobs, JobGroup reads, JobGroup writes, ChunkFunction body,
                           void* context);

  private:
    friend class World;

    Query(World& world, const QueryDesc& desc);

    /* Matches the archetypes that appeared since the last refresh. */
    void refresh();
    void collect(std::vector<detail::Chunk*>& chunks);

    World* world_ = nullptr;
    detail::WorldImpl* impl_ = nullptr;

    std::uint64_t allMask_ = 0;
    std::uint64_t anyMask_ = 0;
    std::uint64_t noneMask_ = 0;

    std::size_t examined_ = 0;
    std::vector<detail::Archetype*> matched_;
    std::vector<detail::Chunk*> chunks_;

    bool valid_ = false;
};

} // namespace resonate::ecs

#endif /* RESONATE_ECS_QUERY_H */
