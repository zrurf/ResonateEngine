#ifndef RESONATE_GAMEPLAY_INTENT_H
#define RESONATE_GAMEPLAY_INTENT_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "resonate/core/allocator.h"
#include "resonate/core/job.h"
#include "resonate/ecs/command_buffer.h"
#include "resonate/ecs/world.h"

namespace resonate::gameplay
{

/*
 * The intent half of the gameplay paradigm: an entity asks the owner of
 * another entity for a change, and the owner's handler adjudicates —
 * change, refuse, or partially apply — at a sync point, never inline.
 *
 * Submission buckets intents by type, FIFO per type. One adjudication is a
 * wave loop: every handler whose type has pending intents runs as one masked
 * job over its type's queue, the pool serializes conflicting handlers, and
 * what handlers submit lands in the next wave. A wave is a barrier, so within
 * one wave the per-type order is the submission order; submissions of one
 * type from threads the masks do not serialize are ordered by the scheduler
 * and therefore replayable only when the submitters conflict on a declared
 * group — the design's determinism triangle (FIFO + masks + RNG streams).
 *
 * The depth cap is the fuse: waves repeat until the queue is quiet or the cap
 * is reached, and what is left spills to the aftermath, which the next
 * simulation step's Phase 0 settles. The bus reports through the world's
 * sink, so a refusal reads like any other ECS report.
 */

using IntentTypeId = std::uint32_t;
inline constexpr IntentTypeId kInvalidIntent = 0xFFFFFFFFU;

/* What a handler receives. The payload is the bytes submitted, valid for the
   call; the endpoints are alive (re-checked at dispatch). */
struct Intent
{
    IntentTypeId type = kInvalidIntent;
    ecs::Entity source{};
    ecs::Entity target{};
    const void* payload = nullptr;
};

/* What a type's intents do when no handler is registered for it. */
enum class UnhandledPolicy : std::uint8_t
{
    Ignore = 0, /* dropped quietly */
    Reject = 1, /* reported once per adjudication, then dropped */
};

class IntentBus
{
  public:
    /* Structural changes go into `commands`, the handler's private recording
       section of the bus's buffer — one per wave dispatch, closed when the
       dispatch returns, merged by the handler's registration order. New
       intents go through submit(). The masks are the systems' resource
       groups: handlers sharing a group with at least one writer are
       serialized, which is also what makes their same-type submissions
       replayable. */
    using Handler = void (*)(void* user, ecs::World& world, ecs::CommandBuffer::Section& commands,
                             const Intent& intent);

    struct HandlerDesc
    {
        IntentTypeId type = kInvalidIntent;
        /* Borrowed: names the handler in reports. */
        const char* name = nullptr;
        Handler run = nullptr;
        void* user = nullptr;
        JobGroup reads = 0;
        JobGroup writes = 0;
    };

    explicit IntentBus(ecs::World& world, Allocator& allocator);
    ~IntentBus();

    IntentBus(const IntentBus&) = delete;
    IntentBus& operator=(const IntentBus&) = delete;

    /* Declares one intent type, before the frame loop. The name is borrowed
       and must outlive the bus. Registering the same name again with the same
       payload size returns the same id; a different size is refused. */
    [[nodiscard]] IntentTypeId registerType(const char* name, std::uint32_t payload_bytes,
                                            UnhandledPolicy unhandled = UnhandledPolicy::Ignore);

    /* Registers one handler, before the frame loop. False, with a report, for
       an unknown type or a null run. */
    bool addHandler(const HandlerDesc& desc);

    /* Queues one intent, copied into the queue. Thread-safe from a stage, a
       parallel body or a wave handler. The endpoints must be alive (a stale
       one is reported and refused); `target` is re-checked at dispatch, where
       a target that died since is dropped with a report. A null payload
       records zero-filled bytes. */
    bool submit(IntentTypeId type, ecs::Entity source, ecs::Entity target, const void* payload);

    /* One adjudication at a sync point: waves until quiet or the cap; a cap
       spill waits in the aftermath for the next step's Phase 0. */
    void adjudicate(JobSystem* jobs);

    /* Phase 0: folds the aftermath of the previous step's spills back into
       the queue, then adjudicates. */
    void settleAftermath(JobSystem* jobs);

    /* Waves one adjudication may run; default 4. */
    void setDepthCap(std::uint32_t waves) noexcept;
    [[nodiscard]] std::uint32_t depthCap() const noexcept;

    /* Intents waiting for the next adjudication, aftermath not counted. */
    [[nodiscard]] std::uint32_t pendingCount() const;

    /* The buffer the handlers record into; the host plays it at the frame's
       sync points like any system's. */
    [[nodiscard]] ecs::CommandBuffer& commands() noexcept;

    /* What the last adjudication did, for tests and the tooling timeline. */
    struct Stats
    {
        std::uint32_t waves = 0;
        std::uint32_t dispatched = 0; /* intents consumed by handlers */
        std::uint32_t spilled = 0;    /* left to the aftermath by the cap */
        std::uint32_t dropped = 0;    /* dead endpoint, or an unhandled reject */
        std::uint32_t settled = 0;    /* aftermath intents folded in (Phase 0) */
    };
    [[nodiscard]] const Stats& lastStats() const noexcept;

  private:
    struct Entry;
    struct TypeInfo;
    struct HandlerEntry;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resonate::gameplay

#endif /* RESONATE_GAMEPLAY_INTENT_H */
