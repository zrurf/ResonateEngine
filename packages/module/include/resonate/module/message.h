#ifndef RESONATE_MODULE_MESSAGE_H
#define RESONATE_MODULE_MESSAGE_H

/*
 * Message streams: notification that may be deferred.
 *
 * The producer owns the stream; a consumer reads it through a cursor it keeps.
 * Payloads are delivered in write order, at a stage boundary the producer
 * chose.
 *
 * `ResonateMessageSequence`, `ResonateMessageCursor` and `ResonateMessageStream`
 * are declared in abi.h, and a module reaches the operations through
 * `ResonateHostApi` — never through this header, which the host uses to
 * implement them.
 */

#include "resonate/module/abi.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateMessageHeader
{
    /* Monotonic and gap-free within a stream; also the trace handle for a message
       and the basis for detecting a consumer that has fallen behind. */
    ResonateMessageSequence sequence;

    /* Producer-defined discriminator; consumers skip what they do not recognise. */
    uint32_t type_id;

    /* Bytes of payload following this header. */
    uint32_t payload_size;
} ResonateMessageHeader;

/*
 * A message type is any trivially copyable struct that carries its own
 * discriminator:
 *
 *   struct AssetLoaded {
 *       static constexpr uint32_t typeId() { return 1; }
 *       ResonateAssetId asset;
 *   };
 *
 * resonate::MessageReader<T> filters on typeId().
 */

typedef struct ResonateMessageStreamVTable
{
    uint32_t struct_size;
    uint32_t thread_mask;

    /* Reads the message after *cursor and advances it.
     *
     * Returns 0 when there is nothing to return, either because the cursor is
     * caught up or because it fell behind the retained window; a consumer tells
     * them apart by comparing its cursor against oldest_sequence.
     *
     * The payload pointer is owned by the stream, aligned for any trivially
     * copyable type, and stays valid only until the next write or reset on that
     * stream; a consumer that keeps it must copy it. */
    uint32_t (*read)(void* self, ResonateMessageCursor* cursor, ResonateMessageHeader* out_header,
                     const void** out_payload);

    /* Oldest sequence still retained. */
    ResonateMessageSequence (*oldest_sequence)(void* self);

    /* Latest sequence written, or 0 when nothing has been written. */
    ResonateMessageSequence (*latest_sequence)(void* self);

    /* Drops everything retained. Cursors are not rewound. */
    void (*reset)(void* self);
} ResonateMessageStreamVTable;

/* Allocated from the host's allocator. Each of slot_count slots holds one record
   up to slot_bytes, header included; a record that does not fit is rejected, not
   truncated. Writing into a full ring evicts the oldest record, so a reader can
   fall at most slot_count records behind. */
ResonateStatus resonate_message_writer_create(ResonateMessageWriter** out_writer,
                                              const ResonateHostApi* host, uint32_t slot_bytes,
                                              uint32_t slot_count);
void resonate_message_writer_destroy(ResonateMessageWriter* writer);

/* Copies the payload into stream-owned storage; the producer's buffer is free
   on return. Returns the assigned sequence, or 0 on failure. */
ResonateMessageSequence resonate_message_write(ResonateMessageWriter* writer, uint32_t type_id,
                                               const void* payload, uint32_t payload_size);

/* Destroys the writer's own readers too, so a module cannot strand one. */
ResonateStatus resonate_message_open_reader(ResonateMessageWriter* writer,
                                            ResonateMessageStream* out_stream);
void resonate_message_close_reader(ResonateMessageStream* stream);

/* Fresh cursor positioned before the oldest retained message. */
ResonateMessageCursor resonate_message_cursor_begin(const ResonateMessageStream* stream);

/* Discards everything; cursors are not rewound. */
void resonate_message_clear(ResonateMessageWriter* writer);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_MODULE_MESSAGE_H */
