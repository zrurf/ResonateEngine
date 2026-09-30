#include <resonate/module/message.h>

#include <cstring>
#include <limits>

#include "host_alloc.h"
#include "resonate/module/capability.h"

/* A ring of fixed slots; a sequence maps to a slot by modulo. */
namespace
{

/* The payload starts right after the header, so a slot's header plus payload
   fills it exactly and the payload keeps the stride's alignment. */
constexpr uint32_t PAYLOAD_OFFSET = static_cast<uint32_t>(sizeof(ResonateMessageHeader));
static_assert(sizeof(ResonateMessageHeader) == 16U, "the payload offset is the header size");

constexpr uint64_t SLOT_ALIGNMENT = 16;

struct ReaderContext
{
    ResonateMessageWriter* writer;

    /* The caller's view, invalidated when the reader closes, so a stream that
       outlives its writer is detectably dead rather than dangling. */
    ResonateMessageStream* stream;

    ReaderContext* next;
    ReaderContext* previous;
};

} // namespace

struct ResonateMessageWriter
{
    const ResonateHostApi* host;
    uint8_t* buffer;
    uint32_t slot_stride;
    uint32_t slot_count;
    uint32_t max_payload;
    ResonateMessageSequence latest;
    ResonateMessageSequence oldest;

    /* Intrusive list: a reader is already an allocation, so the registry costs
       none. Destroying the writer closes whatever is still on it. */
    ReaderContext* readers;
};

namespace
{

uint8_t* slotAt(ResonateMessageWriter* writer, ResonateMessageSequence sequence)
{
    const uint32_t index = static_cast<uint32_t>((sequence - 1U) % writer->slot_count);
    return writer->buffer + (static_cast<size_t>(index) * writer->slot_stride);
}

uint32_t readMessage(void* self, ResonateMessageCursor* cursor, ResonateMessageHeader* out_header,
                     const void** out_payload)
{
    auto* context = static_cast<ReaderContext*>(self);
    ResonateMessageWriter* writer = context->writer;

    const ResonateMessageSequence next = cursor->sequence + 1U;
    if (next < writer->oldest || next > writer->latest)
    {
        return 0;
    }

    const uint8_t* slot = slotAt(writer, next);
    std::memcpy(out_header, slot, sizeof(ResonateMessageHeader));
    *out_payload = slot + PAYLOAD_OFFSET;
    cursor->sequence = next;
    return 1;
}

ResonateMessageSequence oldestSequence(void* self)
{
    return static_cast<ReaderContext*>(self)->writer->oldest;
}

ResonateMessageSequence latestSequence(void* self)
{
    return static_cast<ReaderContext*>(self)->writer->latest;
}

void resetStream(void* self)
{
    ResonateMessageWriter* writer = static_cast<ReaderContext*>(self)->writer;
    writer->oldest = writer->latest + 1U;
}

const ResonateMessageStreamVTable STREAM_VTABLE = {
    sizeof(ResonateMessageStreamVTable),
    RESONATE_THREAD_MAIN,
    &readMessage,
    &oldestSequence,
    &latestSequence,
    &resetStream,
};

/* Unlinks, kills the caller's view and frees; the one path both an explicit
   close and a writer's teardown take. */
void closeReader(ReaderContext* context)
{
    ResonateMessageWriter* writer = context->writer;
    if (context->previous != nullptr)
    {
        context->previous->next = context->next;
    }
    else
    {
        writer->readers = context->next;
    }
    if (context->next != nullptr)
    {
        context->next->previous = context->previous;
    }

    if (context->stream != nullptr)
    {
        context->stream->vtable = nullptr;
        context->stream->self = nullptr;
    }
    resonate::detail::hostDeallocate(writer->host, context, sizeof(ReaderContext));
}

} // namespace

extern "C"
{

ResonateStatus resonate_message_writer_create(ResonateMessageWriter** out_writer,
                                              const ResonateHostApi* host, uint32_t slot_bytes,
                                              uint32_t slot_count)
{
    if (out_writer == nullptr || host == nullptr || slot_count == 0U)
    {
        return RESONATE_E_INVALID;
    }
    *out_writer = nullptr;

    if (slot_bytes <= PAYLOAD_OFFSET)
    {
        return RESONATE_E_INVALID;
    }

    /* Computed in 64 bits: a slot_bytes near UINT32_MAX would wrap the stride to
       zero and hand out a buffer with no room in it. */
    const uint64_t stride =
        (static_cast<uint64_t>(slot_bytes) + (SLOT_ALIGNMENT - 1ULL)) & ~(SLOT_ALIGNMENT - 1ULL);
    if (stride > 0xFFFFFFFFULL || stride > std::numeric_limits<size_t>::max() / slot_count)
    {
        return RESONATE_E_INVALID;
    }

    void* memory = resonate::detail::hostAllocate(host, sizeof(ResonateMessageWriter),
                                                  alignof(ResonateMessageWriter));
    if (memory == nullptr)
    {
        return RESONATE_E_INTERNAL;
    }

    auto* writer = static_cast<ResonateMessageWriter*>(memory);
    writer->host = host;
    writer->slot_stride = static_cast<uint32_t>(stride);
    writer->slot_count = slot_count;
    writer->max_payload = writer->slot_stride - PAYLOAD_OFFSET;
    writer->latest = 0;
    writer->oldest = 1;
    writer->readers = nullptr;

    void* buffer = resonate::detail::hostAllocate(host, static_cast<size_t>(stride) * slot_count,
                                                  SLOT_ALIGNMENT);
    if (buffer == nullptr)
    {
        resonate::detail::hostDeallocate(host, writer, sizeof(ResonateMessageWriter));
        return RESONATE_E_INTERNAL;
    }
    writer->buffer = static_cast<uint8_t*>(buffer);

    *out_writer = writer;
    return RESONATE_OK;
}

void resonate_message_writer_destroy(ResonateMessageWriter* writer)
{
    if (writer == nullptr)
    {
        return;
    }

    /* A module may leave readers open. Closing them here is what keeps a freed
       writer from staying reachable through a stream it still holds. */
    while (writer->readers != nullptr)
    {
        closeReader(writer->readers);
    }

    const ResonateHostApi* host = writer->host;
    resonate::detail::hostDeallocate(host, writer->buffer,
                                     static_cast<size_t>(writer->slot_stride) * writer->slot_count);
    resonate::detail::hostDeallocate(host, writer, sizeof(ResonateMessageWriter));
}

ResonateMessageSequence resonate_message_write(ResonateMessageWriter* writer, uint32_t type_id,
                                               const void* payload, uint32_t payload_size)
{
    if (writer == nullptr || payload_size > writer->max_payload)
    {
        return 0;
    }
    if (payload == nullptr && payload_size != 0U)
    {
        return 0;
    }

    const ResonateMessageSequence sequence = writer->latest + 1U;
    uint8_t* slot = slotAt(writer, sequence);

    const ResonateMessageHeader header = {sequence, type_id, payload_size};
    std::memcpy(slot, &header, sizeof(header));
    if (payload_size != 0U)
    {
        std::memcpy(slot + PAYLOAD_OFFSET, payload, payload_size);
    }

    writer->latest = sequence;
    if (sequence >= writer->oldest + writer->slot_count)
    {
        writer->oldest = sequence - writer->slot_count + 1U;
    }
    return sequence;
}

ResonateStatus resonate_message_open_reader(ResonateMessageWriter* writer,
                                            ResonateMessageStream* out_stream)
{
    if (writer == nullptr || out_stream == nullptr)
    {
        return RESONATE_E_INVALID;
    }

    /* A stream carries one reader: the reader holds the caller's stream so that
       closing invalidates it, and a second open would overwrite that link and
       leave the first reader silently dead. */
    if (out_stream->self != nullptr)
    {
        return RESONATE_E_STATE;
    }

    void* memory =
        resonate::detail::hostAllocate(writer->host, sizeof(ReaderContext), alignof(ReaderContext));
    if (memory == nullptr)
    {
        return RESONATE_E_INTERNAL;
    }

    auto* context = static_cast<ReaderContext*>(memory);
    context->writer = writer;
    context->stream = out_stream;
    context->previous = nullptr;
    context->next = writer->readers;
    if (writer->readers != nullptr)
    {
        writer->readers->previous = context;
    }
    writer->readers = context;

    out_stream->vtable = &STREAM_VTABLE;
    out_stream->self = context;
    return RESONATE_OK;
}

void resonate_message_close_reader(ResonateMessageStream* stream)
{
    if (stream == nullptr || stream->self == nullptr)
    {
        return;
    }

    closeReader(static_cast<ReaderContext*>(stream->self));
}

ResonateMessageCursor resonate_message_cursor_begin(const ResonateMessageStream* stream)
{
    ResonateMessageCursor cursor = {0};
    if (stream != nullptr && stream->self != nullptr)
    {
        const ResonateMessageSequence oldest = stream->vtable->oldest_sequence(stream->self);
        cursor.sequence = oldest > 0U ? oldest - 1U : 0U;
    }
    return cursor;
}

void resonate_message_clear(ResonateMessageWriter* writer)
{
    if (writer == nullptr)
    {
        return;
    }
    writer->oldest = writer->latest + 1U;
}

} // extern "C"
