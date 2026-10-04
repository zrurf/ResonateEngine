#include <resonate/pal/thread.h>

#include <new>

#include <windows.h>

#include "../status.h"

namespace
{

/* CreateThread takes one parameter, so the entry and its argument travel
   together. */
struct ThreadStart
{
    ResonateThreadEntry entry;
    void* argument;
    int32_t affinity;
};

/* Initialised during CRT startup, which runs on the main thread. */
const uint32_t MAIN_THREAD_ID = GetCurrentThreadId();

/* An index outside the mask would shift past the end of the word. */
bool coreInRange(int32_t core)
{
    return core < static_cast<int32_t>(8U * sizeof(DWORD_PTR));
}

/* Placement can only be applied to the thread being placed, which is why this
   runs on the new thread rather than on the one that created it. */
void applyAffinity(int32_t core)
{
    SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << core);
}

DWORD WINAPI trampoline(LPVOID parameter)
{
    auto* start = static_cast<ThreadStart*>(parameter);
    const ResonateThreadEntry entry = start->entry;
    void* argument = start->argument;
    const int32_t affinity = start->affinity;
    delete start;

    if (affinity >= 0)
    {
        applyAffinity(affinity);
    }

    entry(argument);
    return 0;
}

} // namespace

extern "C"
{

ResonatePalStatus resonate_pal_thread_create(ResonateThreadHandle* out_thread,
                                             ResonateThreadEntry entry, void* argument,
                                             uint32_t stack_size, int32_t affinity)
{
    if (out_thread == nullptr || entry == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    if (affinity >= 0 && !coreInRange(affinity))
    {
        return RESONATE_PAL_INVALID;
    }

    auto* start = new (std::nothrow) ThreadStart{entry, argument, affinity};
    if (start == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    HANDLE thread =
        CreateThread(nullptr, static_cast<SIZE_T>(stack_size), &trampoline, start, 0, nullptr);
    if (thread == nullptr)
    {
        delete start;
        return resonate::pal::windows::statusFromLastError();
    }

    out_thread->handle = thread;
    return RESONATE_PAL_OK;
}

void resonate_pal_thread_destroy(ResonateThreadHandle* thread)
{
    if (thread == nullptr || thread->handle == nullptr)
    {
        return;
    }
    CloseHandle(static_cast<HANDLE>(thread->handle));
    thread->handle = nullptr;
}

ResonatePalStatus resonate_pal_thread_join(ResonateThreadHandle* thread)
{
    if (thread == nullptr || thread->handle == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    const DWORD result = WaitForSingleObject(static_cast<HANDLE>(thread->handle), INFINITE);
    if (result != WAIT_OBJECT_0)
    {
        return resonate::pal::windows::statusFromWin32(WAIT_TIMEOUT);
    }

    /* The thread has ended, so the handle is released here and a later destroy
       finds nothing to close. */
    CloseHandle(static_cast<HANDLE>(thread->handle));
    thread->handle = nullptr;
    return RESONATE_PAL_OK;
}

void resonate_pal_thread_yield(void)
{
    SwitchToThread();
}

void resonate_pal_thread_set_priority(ResonateThreadPriority priority)
{
    int value = THREAD_PRIORITY_NORMAL;
    switch (priority)
    {
        case RESONATE_PAL_THREAD_PRIORITY_LOW:
            value = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case RESONATE_PAL_THREAD_PRIORITY_HIGH:
            value = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        default:
            break;
    }
    SetThreadPriority(GetCurrentThread(), value);
}

uint32_t resonate_pal_thread_current_cpu(void)
{
    return static_cast<uint32_t>(GetCurrentProcessorNumber());
}

uint32_t resonate_pal_thread_id(void)
{
    return GetCurrentThreadId();
}

uint32_t resonate_pal_thread_is_main(void)
{
    return GetCurrentThreadId() == MAIN_THREAD_ID ? 1U : 0U;
}

} // extern "C"
