#include <resonate/pal/event.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

#include <resonate/pal/chrono.h>

#include "../file_descriptor.h"
#include "../status.h"
#include "wakeup.h"

/* The kernel reports no completion for an ordinary file, so a worker thread runs
   the submission and queues its completion for the dispatch thread. The wakeup
   descriptor is what a blocked dispatch waits on. */
namespace
{

constexpr uint32_t MAX_WORKERS = 4;

struct Request
{
    int descriptor;
    ResonateEventToken token;
    ResonateEventOperation operation;
    void* buffer;
    uint32_t size;
    uint64_t offset;
};

struct Completion
{
    ResonateEventToken token;
    ResonateEventOperation operation;
    ResonatePalStatus status;
    uint32_t bytes_transferred;
    void* buffer;
};

struct Loop
{
    resonate::pal::posix::Wakeup wakeup;
    std::mutex mutex;
    std::condition_variable work_ready;
    std::deque<Request> pending;
    std::deque<Completion> completions;
    std::vector<std::thread> workers;
    bool stopping = false;
};

/* pread and pwrite can transfer less than asked and can be interrupted. */
Completion run(const Request& request)
{
    Completion completion = {};
    completion.token = request.token;
    completion.operation = request.operation;
    completion.buffer = request.buffer;

    auto* cursor = static_cast<unsigned char*>(request.buffer);
    uint64_t total = 0;

    while (total < request.size)
    {
        const uint64_t remaining = request.size - total;
        const uint64_t position = request.offset + total;

        ssize_t transferred = 0;
        if (request.operation == RESONATE_EVENT_OP_FILE_READ)
        {
            transferred =
                pread(request.descriptor, cursor + total, remaining, static_cast<off_t>(position));
        }
        else
        {
            transferred =
                pwrite(request.descriptor, cursor + total, remaining, static_cast<off_t>(position));
        }

        if (transferred < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            completion.status = resonate::pal::posix::statusFromLastErrno();
            return completion;
        }
        if (transferred == 0)
        {
            break;
        }
        total += static_cast<uint64_t>(transferred);
    }

    completion.status = RESONATE_PAL_OK;
    completion.bytes_transferred = static_cast<uint32_t>(total);
    return completion;
}

void worker(Loop* loop)
{
    while (true)
    {
        Request request = {};
        {
            std::unique_lock<std::mutex> guard(loop->mutex);
            loop->work_ready.wait(guard,
                                  [loop] { return loop->stopping || !loop->pending.empty(); });
            if (loop->pending.empty())
            {
                return;
            }
            request = loop->pending.front();
            loop->pending.pop_front();
        }

        const Completion completion = run(request);

        {
            const std::lock_guard<std::mutex> guard(loop->mutex);
            loop->completions.push_back(completion);
        }
        resonate::pal::posix::wakeupSignal(loop->wakeup);
    }
}

void takeCompletions(Loop* loop, std::deque<Completion>& out)
{
    const std::lock_guard<std::mutex> guard(loop->mutex);
    out.swap(loop->completions);
}

ResonatePalStatus submit(ResonateEventLoop* loop, ResonateEventToken token,
                         const ResonateFileHandle* file, const void* buffer, uint32_t size,
                         uint64_t offset, ResonateEventOperation operation)
{
    if (loop == nullptr || loop->handle == nullptr || file == nullptr || file->handle == nullptr ||
        buffer == nullptr || size == 0U)
    {
        return RESONATE_PAL_INVALID;
    }

    auto* state = static_cast<Loop*>(loop->handle);
    Request request = {};
    request.descriptor = resonate::pal::posix::descriptorFromHandle(file);
    request.token = token;
    request.operation = operation;
    request.buffer = const_cast<void*>(buffer);
    request.size = size;
    request.offset = offset;

    {
        const std::lock_guard<std::mutex> guard(state->mutex);
        state->pending.push_back(request);
    }
    state->work_ready.notify_one();
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

    auto* loop = new (std::nothrow) Loop();
    if (loop == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    loop->wakeup = resonate::pal::posix::wakeupCreate();
    if (loop->wakeup.wait < 0)
    {
        const ResonatePalStatus status = resonate::pal::posix::statusFromLastErrno();
        delete loop;
        return status;
    }

    const uint32_t available = std::thread::hardware_concurrency();
    const uint32_t workers = std::max(1U, std::min(MAX_WORKERS, available));
    try
    {
        loop->workers.reserve(workers);
        for (uint32_t index = 0; index < workers; ++index)
        {
            loop->workers.emplace_back(&worker, loop);
        }
    }
    catch (const std::system_error&)
    {
        {
            const std::lock_guard<std::mutex> guard(loop->mutex);
            loop->stopping = true;
        }
        loop->work_ready.notify_all();
        for (std::thread& thread : loop->workers)
        {
            thread.join();
        }
        resonate::pal::posix::wakeupDestroy(loop->wakeup);
        delete loop;
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    out_loop->handle = loop;
    return RESONATE_PAL_OK;
}

/* Stops the workers, then discards whatever they had queued. */
void resonate_pal_event_destroy_loop(ResonateEventLoop* loop)
{
    if (loop == nullptr || loop->handle == nullptr)
    {
        return;
    }

    auto* state = static_cast<Loop*>(loop->handle);
    {
        const std::lock_guard<std::mutex> guard(state->mutex);
        state->stopping = true;
    }
    state->work_ready.notify_all();

    for (std::thread& thread : state->workers)
    {
        thread.join();
    }

    resonate::pal::posix::wakeupDestroy(state->wakeup);
    delete state;
    loop->handle = nullptr;
}

ResonatePalStatus resonate_pal_event_submit_read(ResonateEventLoop* loop, ResonateEventToken token,
                                                 const ResonateFileHandle* file, void* buffer,
                                                 uint32_t size, uint64_t offset)
{
    return submit(loop, token, file, buffer, size, offset, RESONATE_EVENT_OP_FILE_READ);
}

ResonatePalStatus resonate_pal_event_submit_write(ResonateEventLoop* loop, ResonateEventToken token,
                                                  const ResonateFileHandle* file,
                                                  const void* buffer, uint32_t size,
                                                  uint64_t offset)
{
    return submit(loop, token, file, buffer, size, offset, RESONATE_EVENT_OP_FILE_WRITE);
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
    const uint64_t started = resonate_pal_chrono_ticks();
    const uint64_t budget = resonate_pal_chrono_nanoseconds_to_ticks(timeout_nanoseconds);

    std::deque<Completion> ready;
    while (ready.empty())
    {
        takeCompletions(state, ready);
        if (!ready.empty())
        {
            break;
        }

        const uint64_t spent = resonate_pal_chrono_ticks() - started;
        if (spent >= budget)
        {
            break;
        }

        /* A worker queues its completion before signalling, so a wakeup that finds
           nothing to take is either a signal an earlier dispatch left behind — one
           that took its completion without ever waiting — or a handled signal that
           cut the wait short. Waiting again for what is left of the timeout is what
           keeps either from being reported as "nothing was ready" while a
           completion is on its way. */
        const int signalled = resonate::pal::posix::wakeupWait(
            state->wakeup, resonate_pal_chrono_ticks_to_nanoseconds(budget - spent));
        if (signalled == resonate::pal::posix::WAKEUP_FAILED)
        {
            return resonate::pal::posix::statusFromLastErrno();
        }
        if (signalled == resonate::pal::posix::WAKEUP_EXPIRED)
        {
            break;
        }

        resonate::pal::posix::wakeupDrain(state->wakeup);
    }

    uint32_t dispatched = 0;
    for (const Completion& queued : ready)
    {
        ResonateEventCompletion completion = {};
        completion.token = queued.token;
        completion.operation = queued.operation;
        completion.status = queued.status;
        completion.bytes_transferred = queued.bytes_transferred;
        completion.buffer = queued.buffer;
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
