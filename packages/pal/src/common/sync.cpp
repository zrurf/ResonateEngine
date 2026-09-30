#include <resonate/pal/sync.h>

#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

/* Handles wrap the standard library's primitives: PAL promises behaviour, not a
   particular implementation, and SRWLOCK or pthread_mutex would add nothing a
   caller can observe. */

namespace
{

struct MutexImpl
{
    explicit MutexImpl(bool is_recursive) : recursive(is_recursive)
    {
    }

    bool recursive = false;
    std::mutex plain;
    std::recursive_mutex nested;
};

/* A change counter around a plain condition_variable: the caller's mutex is
   released and reacquired here, not by the standard library. */
struct ConditionImpl
{
    std::mutex internal;
    std::condition_variable changed;
    uint64_t changes = 0;
};

/* Not std::counting_semaphore: its count is capped at an implementation-defined
   maximum below what a uint32_t count can ask for. */
struct SemaphoreImpl
{
    std::mutex mutex;
    std::condition_variable condition;
    uint32_t count = 0;
};

} // namespace

extern "C"
{

ResonatePalStatus resonate_pal_sync_create_mutex(ResonateMutex* out_mutex, int recursive)
{
    if (out_mutex == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    auto* implementation = new (std::nothrow) MutexImpl(recursive != 0);
    if (implementation == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    out_mutex->handle = implementation;
    return RESONATE_PAL_OK;
}

void resonate_pal_sync_destroy_mutex(ResonateMutex* mutex)
{
    if (mutex == nullptr)
    {
        return;
    }
    delete static_cast<MutexImpl*>(mutex->handle);
    mutex->handle = nullptr;
}

void resonate_pal_sync_lock_mutex(ResonateMutex* mutex)
{
    if (mutex == nullptr || mutex->handle == nullptr)
    {
        return;
    }

    auto* implementation = static_cast<MutexImpl*>(mutex->handle);
    if (implementation->recursive)
    {
        implementation->nested.lock();
    }
    else
    {
        implementation->plain.lock();
    }
}

void resonate_pal_sync_unlock_mutex(ResonateMutex* mutex)
{
    if (mutex == nullptr || mutex->handle == nullptr)
    {
        return;
    }

    auto* implementation = static_cast<MutexImpl*>(mutex->handle);
    if (implementation->recursive)
    {
        implementation->nested.unlock();
    }
    else
    {
        implementation->plain.unlock();
    }
}

ResonatePalStatus resonate_pal_sync_create_condition(ResonateConditionVariable* out_condition)
{
    if (out_condition == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    auto* implementation = new (std::nothrow) ConditionImpl();
    if (implementation == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    out_condition->handle = implementation;
    return RESONATE_PAL_OK;
}

void resonate_pal_sync_destroy_condition(ResonateConditionVariable* condition)
{
    if (condition == nullptr)
    {
        return;
    }
    delete static_cast<ConditionImpl*>(condition->handle);
    condition->handle = nullptr;
}

/* The caller holds mutex. It is released for the duration of the wait and
   reacquired before returning. */
void resonate_pal_sync_wait(ResonateConditionVariable* condition, ResonateMutex* mutex)
{
    if (condition == nullptr || condition->handle == nullptr || mutex == nullptr ||
        mutex->handle == nullptr)
    {
        return;
    }

    auto* implementation = static_cast<ConditionImpl*>(condition->handle);
    auto* lock = static_cast<MutexImpl*>(mutex->handle);

    uint64_t observed = 0;
    {
        const std::lock_guard<std::mutex> guard(implementation->internal);
        observed = implementation->changes;
    }

    if (lock->recursive)
    {
        lock->nested.unlock();
    }
    else
    {
        lock->plain.unlock();
    }

    {
        std::unique_lock<std::mutex> guard(implementation->internal);
        implementation->changed.wait(guard, [implementation, observed]
                                     { return implementation->changes != observed; });
    }

    if (lock->recursive)
    {
        lock->nested.lock();
    }
    else
    {
        lock->plain.lock();
    }
}

void resonate_pal_sync_signal(ResonateConditionVariable* condition)
{
    if (condition == nullptr || condition->handle == nullptr)
    {
        return;
    }

    auto* implementation = static_cast<ConditionImpl*>(condition->handle);
    {
        const std::lock_guard<std::mutex> guard(implementation->internal);
        ++implementation->changes;
    }
    implementation->changed.notify_one();
}

void resonate_pal_sync_broadcast(ResonateConditionVariable* condition)
{
    if (condition == nullptr || condition->handle == nullptr)
    {
        return;
    }

    auto* implementation = static_cast<ConditionImpl*>(condition->handle);
    {
        const std::lock_guard<std::mutex> guard(implementation->internal);
        ++implementation->changes;
    }
    implementation->changed.notify_all();
}

ResonatePalStatus resonate_pal_sync_create_read_write_lock(ResonateReadWriteLock* out_lock)
{
    if (out_lock == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    auto* implementation = new (std::nothrow) MutexImpl(false);
    if (implementation == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }

    out_lock->handle = implementation;
    return RESONATE_PAL_OK;
}

void resonate_pal_sync_destroy_read_write_lock(ResonateReadWriteLock* lock)
{
    if (lock == nullptr)
    {
        return;
    }
    delete static_cast<MutexImpl*>(lock->handle);
    lock->handle = nullptr;
}

/* Shared and exclusive take the same lock: there is no reader concurrency. */
void resonate_pal_sync_lock_shared(ResonateReadWriteLock* lock)
{
    if (lock == nullptr || lock->handle == nullptr)
    {
        return;
    }
    static_cast<MutexImpl*>(lock->handle)->plain.lock();
}

void resonate_pal_sync_lock_exclusive(ResonateReadWriteLock* lock)
{
    resonate_pal_sync_lock_shared(lock);
}

void resonate_pal_sync_unlock_read_write_lock(ResonateReadWriteLock* lock)
{
    if (lock == nullptr || lock->handle == nullptr)
    {
        return;
    }
    static_cast<MutexImpl*>(lock->handle)->plain.unlock();
}

ResonatePalStatus resonate_pal_sync_create_semaphore(ResonateSemaphore* out_semaphore,
                                                     uint32_t initial_count)
{
    if (out_semaphore == nullptr)
    {
        return RESONATE_PAL_INVALID;
    }

    auto* implementation = new (std::nothrow) SemaphoreImpl();
    if (implementation == nullptr)
    {
        return RESONATE_PAL_OUT_OF_MEMORY;
    }
    implementation->count = initial_count;

    out_semaphore->handle = implementation;
    return RESONATE_PAL_OK;
}

void resonate_pal_sync_destroy_semaphore(ResonateSemaphore* semaphore)
{
    if (semaphore == nullptr)
    {
        return;
    }
    delete static_cast<SemaphoreImpl*>(semaphore->handle);
    semaphore->handle = nullptr;
}

void resonate_pal_sync_wait_semaphore(ResonateSemaphore* semaphore)
{
    if (semaphore == nullptr || semaphore->handle == nullptr)
    {
        return;
    }

    auto* implementation = static_cast<SemaphoreImpl*>(semaphore->handle);
    std::unique_lock<std::mutex> guard(implementation->mutex);
    implementation->condition.wait(guard, [implementation] { return implementation->count > 0U; });
    --implementation->count;
}

void resonate_pal_sync_signal_semaphore(ResonateSemaphore* semaphore, uint32_t count)
{
    if (semaphore == nullptr || semaphore->handle == nullptr)
    {
        return;
    }

    auto* implementation = static_cast<SemaphoreImpl*>(semaphore->handle);
    {
        const std::lock_guard<std::mutex> guard(implementation->mutex);
        implementation->count += count;
    }
    implementation->condition.notify_all();
}

uint32_t resonate_pal_sync_hardware_concurrency(void)
{
    const unsigned int count = std::thread::hardware_concurrency();
    return count > 0U ? count : 1U;
}

} // extern "C"
