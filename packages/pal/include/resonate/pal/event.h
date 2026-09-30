#ifndef RESONATE_PAL_EVENT_H
#define RESONATE_PAL_EVENT_H

/*
 * Asynchronous I/O event loop: one loop per process, shared by the asset
 * pipeline and the network stack.
 *
 * Completions are delivered to the dispatch thread, never from inside submit. A
 * submission names a file from the io layer, opened with
 * RESONATE_FILE_OVERLAPPED: an overlapped operation needs a file handle, not a
 * descriptor.
 *
 * Submission is thread-safe. Dispatch is not: exactly one thread calls
 * resonate_pal_event_dispatch at a time.
 */

#include <stddef.h>
#include <stdint.h>

#include "resonate/pal/io.h"
#include "resonate/pal/status.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateEventLoop
{
    void* handle;
} ResonateEventLoop;

typedef uint64_t ResonateEventToken;

typedef enum ResonateEventOperation : uint8_t
{
    RESONATE_EVENT_OP_NONE = 0,
    RESONATE_EVENT_OP_FILE_READ = 1,
    RESONATE_EVENT_OP_FILE_WRITE = 2
} ResonateEventOperation;

/* One completed operation. status is RESONATE_PAL_OK on success, and
   bytes_transferred is meaningless when it is not. The token is the caller's own
   identity for the submission, which is what a callback dispatches on. */
typedef struct ResonateEventCompletion
{
    ResonateEventToken token;
    ResonateEventOperation operation;
    ResonatePalStatus status;
    uint32_t bytes_transferred;
    void* buffer;
} ResonateEventCompletion;

typedef void (*ResonateEventCallback)(const ResonateEventCompletion* completion);

ResonatePalStatus resonate_pal_event_create_loop(ResonateEventLoop* out_loop);
void resonate_pal_event_destroy_loop(ResonateEventLoop* loop);

/* Queues one operation on an open file. The buffer and the file must outlive the
   completion, which is delivered to a later dispatch call; the loop does not
   copy either. A failure here means nothing was queued and no completion will
   arrive. */
ResonatePalStatus resonate_pal_event_submit_read(ResonateEventLoop* loop, ResonateEventToken token,
                                                 const ResonateFileHandle* file, void* buffer,
                                                 uint32_t size, uint64_t offset);
ResonatePalStatus resonate_pal_event_submit_write(ResonateEventLoop* loop, ResonateEventToken token,
                                                  const ResonateFileHandle* file,
                                                  const void* buffer, uint32_t size,
                                                  uint64_t offset);

/* Waits up to timeout_nanoseconds for activity, then invokes the callback once
   per completion that is ready by then. out_dispatched counts the callbacks that
   ran. RESONATE_PAL_TIMEOUT means nothing was ready, and is a normal result. */
ResonatePalStatus resonate_pal_event_dispatch(ResonateEventLoop* loop,
                                              ResonateEventCallback callback,
                                              uint64_t timeout_nanoseconds,
                                              uint32_t* out_dispatched);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_EVENT_H */
