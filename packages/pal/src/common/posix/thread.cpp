#include <resonate/pal/thread.h>

#include <cerrno>
#include <cstdint>
#include <new>

#include <pthread.h>

#if defined(__APPLE__)
#    include <mach/mach.h>
#    include <mach/thread_policy.h>
#else
#    include <sched.h>
#    include <sys/syscall.h>
#    include <unistd.h>
#endif

#include "status.h"

namespace
{

/* pthread_create takes one parameter, so the entry and its argument travel
   together in a block the thread owns. */
struct ThreadStart
{
    ResonateThreadEntry entry;
    void* argument;
    int32_t affinity;
};

/* Captured during static initialisation, on the main thread. */
const pthread_t MAIN_THREAD = pthread_self();

/* An index outside the set would walk off the end of the cpu_set_t, so it is
   refused before any thread exists. */
bool coreInRange(int32_t core)
{
#if defined(__APPLE__)
    /* The Apple policy takes an arbitrary tag rather than a core index. */
    return true;
#else
    return core < CPU_SETSIZE;
#endif
}

/* Placement can only be applied to the thread being placed:
   pthread_setaffinity_np acts on the calling thread, which is why this runs on
   the new thread rather than on the one that created it. */
void applyAffinity(int32_t core)
{
#if defined(__APPLE__)
    thread_affinity_policy_data_t policy = {core};
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_AFFINITY_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), 1);
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(core), &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

void* trampoline(void* parameter)
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
    return nullptr;
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
    out_thread->handle = nullptr;

    if (affinity >= 0 && !coreInRange(affinity))
    {
        return RESONATE_PAL_INVALID;
    }

    auto* start = new (std::nothrow) ThreadStart{entry, argument, affinity};
    if (start == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    if (stack_size != 0U)
    {
        pthread_attr_setstacksize(&attributes, stack_size);
    }

    pthread_t thread = {};
    const int created = pthread_create(&thread, &attributes, &trampoline, start);
    pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        delete start;
        return resonate::pal::posix::statusFromErrno(created);
    }

    out_thread->handle = reinterpret_cast<void*>(thread);
    return RESONATE_PAL_OK;
}

/* Detaches: a handle may be released before the thread is joined. */
void resonate_pal_thread_destroy(ResonateThreadHandle* thread)
{
    if (thread == nullptr || thread->handle == nullptr)
    {
        return;
    }

    pthread_detach(reinterpret_cast<pthread_t>(thread->handle));
    thread->handle = nullptr;
}

ResonatePalStatus resonate_pal_thread_join(ResonateThreadHandle* thread)
{
    if (thread == nullptr || thread->handle == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    const int joined = pthread_join(reinterpret_cast<pthread_t>(thread->handle), nullptr);
    if (joined != 0)
    {
        return resonate::pal::posix::statusFromErrno(joined);
    }

    /* Joined: the id is no longer safe to name, and detaching an already joined
       thread is undefined, so the handle is cleared here rather than left for
       destroy to act on. */
    thread->handle = nullptr;
    return RESONATE_PAL_OK;
}

void resonate_pal_thread_yield(void)
{
    sched_yield();
}

uint32_t resonate_pal_thread_id(void)
{
#if defined(__APPLE__)
    return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#else
    return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

uint32_t resonate_pal_thread_is_main(void)
{
    return pthread_equal(pthread_self(), MAIN_THREAD) != 0 ? 1U : 0U;
}

} // extern "C"
