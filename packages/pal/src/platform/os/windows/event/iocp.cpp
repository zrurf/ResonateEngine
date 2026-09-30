#include <resonate/pal/event.h>

#include <mutex>
#include <new>

#include <windows.h>

#include "../status.h"

/* Completion port. Files are opened with RESONATE_FILE_OVERLAPPED and associated
   on first submission; an operation that completes before ReadFile returns still
   queues its packet, so every completion takes this one path. */
namespace
{

struct Request
{
    /* First member: the port hands it back as an OVERLAPPED pointer. */
    OVERLAPPED overlapped;

    HANDLE file;
    ResonateEventToken token;
    ResonateEventOperation operation;
    void* buffer;
    uint32_t size;

    Request* previous;
    Request* next;
};

struct Loop
{
    HANDLE port;
    Request* pending;

    /* Submission is thread-safe while dispatch is not, so the list has to hold up
       under one thread tracking while another untracks. */
    std::mutex mutex;
};

DWORD timeoutMilliseconds(uint64_t nanoseconds)
{
    if (nanoseconds == 0U)
    {
        return 0;
    }
    const uint64_t milliseconds = nanoseconds / 1000000ULL;
    if (milliseconds == 0U)
    {
        return 1;
    }
    return milliseconds > 0xFFFFFFFEULL ? INFINITE : static_cast<DWORD>(milliseconds);
}

void track(Loop* loop, Request* request)
{
    request->previous = nullptr;
    request->next = loop->pending;
    if (loop->pending != nullptr)
    {
        loop->pending->previous = request;
    }
    loop->pending = request;
}

void untrack(Loop* loop, Request* request)
{
    if (request->previous != nullptr)
    {
        request->previous->next = request->next;
    }
    else
    {
        loop->pending = request->next;
    }
    if (request->next != nullptr)
    {
        request->next->previous = request->previous;
    }
}

void trackLocked(Loop* loop, Request* request)
{
    const std::lock_guard<std::mutex> guard(loop->mutex);
    track(loop, request);
}

void untrackLocked(Loop* loop, Request* request)
{
    const std::lock_guard<std::mutex> guard(loop->mutex);
    untrack(loop, request);
}

ResonatePalStatus submit(ResonateEventLoop* loop, ResonateEventToken token,
                         const ResonateFileHandle* file, void* buffer, uint32_t size,
                         uint64_t offset, ResonateEventOperation operation, bool writing)
{
    if (loop == nullptr || loop->handle == nullptr || file == nullptr || file->handle == nullptr ||
        buffer == nullptr || size == 0U)
    {
        return RESONATE_PAL_INVALID;
    }

    auto* state = static_cast<Loop*>(loop->handle);
    auto* request = new (std::nothrow) Request{};
    if (request == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    request->file = static_cast<HANDLE>(file->handle);
    request->token = token;
    request->operation = operation;
    request->buffer = buffer;
    request->size = size;
    request->overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    request->overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);

    /* Re-associating a handle that this loop already owns fails with
       ERROR_INVALID_PARAMETER, which is not an error here: the earlier
       association still holds. A handle associated with a different port is
       outside the contract of one loop per process. */
    if (CreateIoCompletionPort(request->file, state->port, 0, 0) == nullptr &&
        GetLastError() != ERROR_INVALID_PARAMETER)
    {
        const ResonatePalStatus status = resonate::pal::windows::statusFromLastError();
        delete request;
        return status;
    }

    trackLocked(state, request);

    const BOOL started = writing
                             ? WriteFile(request->file, buffer, size, nullptr, &request->overlapped)
                             : ReadFile(request->file, buffer, size, nullptr, &request->overlapped);
    if (started == 0)
    {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING)
        {
            untrackLocked(state, request);
            delete request;
            return resonate::pal::windows::statusFromWin32(error);
        }
    }

    return RESONATE_PAL_OK;
}

} // namespace

extern "C"
{

ResonatePalStatus resonate_pal_event_create_loop(ResonateEventLoop* out_loop)
{
    if (out_loop == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }
    out_loop->handle = nullptr;

    auto* state = new (std::nothrow) Loop{};
    if (state == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    state->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (state->port == nullptr)
    {
        const ResonatePalStatus status = resonate::pal::windows::statusFromLastError();
        delete state;
        return status;
    }

    out_loop->handle = state;
    return RESONATE_PAL_OK;
}

/* Cancels what is still in flight and discards the packets, so no request
   outlives the loop. */
void resonate_pal_event_destroy_loop(ResonateEventLoop* loop)
{
    if (loop == nullptr || loop->handle == nullptr)
    {
        return;
    }

    auto* state = static_cast<Loop*>(loop->handle);

    /* Cancel what is in flight, then collect the packets that produces. */
    {
        const std::lock_guard<std::mutex> guard(state->mutex);
        for (Request* request = state->pending; request != nullptr; request = request->next)
        {
            CancelIoEx(request->file, &request->overlapped);
        }
    }

    while (state->pending != nullptr)
    {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        if (GetQueuedCompletionStatus(state->port, &bytes, &key, &overlapped, 100) == 0 &&
            overlapped == nullptr)
        {
            break;
        }
        if (overlapped != nullptr)
        {
            auto* request = reinterpret_cast<Request*>(overlapped);
            untrackLocked(state, request);
            delete request;
        }
    }

    /* A cancelled operation whose packet never arrives would otherwise outlive the
       loop, holding a caller's buffer. */
    {
        const std::lock_guard<std::mutex> guard(state->mutex);
        while (state->pending != nullptr)
        {
            Request* const request = state->pending;
            untrack(state, request);
            delete request;
        }
    }

    CloseHandle(state->port);
    delete state;
    loop->handle = nullptr;
}

ResonatePalStatus resonate_pal_event_submit_read(ResonateEventLoop* loop, ResonateEventToken token,
                                                 const ResonateFileHandle* file, void* buffer,
                                                 uint32_t size, uint64_t offset)
{
    return submit(loop, token, file, buffer, size, offset, RESONATE_EVENT_OP_FILE_READ, false);
}

ResonatePalStatus resonate_pal_event_submit_write(ResonateEventLoop* loop, ResonateEventToken token,
                                                  const ResonateFileHandle* file,
                                                  const void* buffer, uint32_t size,
                                                  uint64_t offset)
{
    return submit(loop, token, file, const_cast<void*>(buffer), size, offset,
                  RESONATE_EVENT_OP_FILE_WRITE, true);
}

ResonatePalStatus resonate_pal_event_dispatch(ResonateEventLoop* loop,
                                              ResonateEventCallback callback,
                                              uint64_t timeout_nanoseconds,
                                              uint32_t* out_dispatched)
{
    if (loop == nullptr || loop->handle == nullptr || callback == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    auto* state = static_cast<Loop*>(loop->handle);
    uint32_t dispatched = 0;
    DWORD wait = timeoutMilliseconds(timeout_nanoseconds);

    while (true)
    {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL completed =
            GetQueuedCompletionStatus(state->port, &bytes, &key, &overlapped, wait);
        wait = 0;

        if (overlapped == nullptr)
        {
            break;
        }

        auto* request = reinterpret_cast<Request*>(overlapped);
        untrackLocked(state, request);

        ResonateEventCompletion completion = {};
        completion.token = request->token;
        completion.operation = request->operation;
        completion.status =
            completed != 0 ? RESONATE_PAL_OK : resonate::pal::windows::statusFromLastError();
        completion.bytes_transferred = completed != 0 ? bytes : 0U;
        completion.buffer = request->buffer;

        delete request;

        callback(&completion);
        ++dispatched;
    }

    if (out_dispatched != nullptr)
    {
        *out_dispatched = dispatched;
    }
    return dispatched > 0U ? RESONATE_PAL_OK : RESONATE_PAL_TIMEOUT;
}

} // extern "C"
