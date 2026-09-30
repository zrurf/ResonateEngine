#include <catch2/catch_all.hpp>

#include <atomic>
#include <cstdint>
#include <string>

#include <resonate/pal/pal.h>

namespace
{

constexpr const char* TEST_FILE = "resonate_pal_test.bin";
constexpr const char* TEST_FILE_RENAMED = "resonate_pal_test_renamed.bin";
constexpr const char* TEST_DIRECTORY = "resonate_pal_test_dir";

void removeIfPresent(const char* path)
{
    if (resonate_pal_io_exists(path) != 0U)
    {
        resonate_pal_io_remove(path);
    }
}

} // namespace

TEST_CASE("the clock only moves forward and converts consistently", "[pal][chrono]")
{
    const ResonateClockInfo info = resonate_pal_chrono_clock_info();
    REQUIRE(info.frequency > 0U);

    const uint64_t before = resonate_pal_chrono_ticks();
    const uint64_t nanoseconds = resonate_pal_chrono_ticks_to_nanoseconds(before);
    const uint64_t back = resonate_pal_chrono_nanoseconds_to_ticks(nanoseconds);

    /* The round trip loses at most one tick to integer division. */
    const uint64_t difference = back > before ? back - before : before - back;
    REQUIRE(difference <= 1U);

    resonate_pal_chrono_sleep(2000000ULL);

    const uint64_t after = resonate_pal_chrono_ticks();
    REQUIRE(after > before);
    REQUIRE(resonate_pal_chrono_now().nanoseconds >=
            resonate_pal_chrono_ticks_to_nanoseconds(before));
}

TEST_CASE("memory is accounted and aligned", "[pal][memory]")
{
    ResonateMemoryStats before = {};
    resonate_pal_memory_get_stats(&before);

    void* block = nullptr;
    REQUIRE(resonate_pal_memory_allocate(&block, 1000U, 64U) == RESONATE_PAL_OK);
    REQUIRE(block != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(block) % 64U == 0U);

    ResonateMemoryStats during = {};
    resonate_pal_memory_get_stats(&during);
    REQUIRE(during.live_blocks == before.live_blocks + 1U);
    REQUIRE(during.allocated_bytes >= before.allocated_bytes + 1000U);
    REQUIRE(during.peak_bytes >= during.allocated_bytes);

    void* grown = nullptr;
    REQUIRE(resonate_pal_memory_reallocate(&grown, block, 1000U, 2000U, 64U) == RESONATE_PAL_OK);
    REQUIRE(grown != nullptr);

    resonate_pal_memory_deallocate(grown, 2000U);

    /* An alignment a pointer cannot be rounded to is refused rather than served
       misaligned, which is what a masking back end does with one. */
    REQUIRE(resonate_pal_memory_allocate(&block, 64U, 24U) == RESONATE_PAL_INVALID);

    ResonateMemoryStats after = {};
    resonate_pal_memory_get_stats(&after);
    REQUIRE(after.live_blocks == before.live_blocks);
}

TEST_CASE("a file survives a write and read round trip", "[pal][io]")
{
    removeIfPresent(TEST_FILE);
    removeIfPresent(TEST_FILE_RENAMED);

    ResonateFileHandle file = {};
    REQUIRE(resonate_pal_io_open_file(&file, TEST_FILE,
                                      RESONATE_FILE_READ | RESONATE_FILE_WRITE |
                                          RESONATE_FILE_CREATE | RESONATE_FILE_TRUNCATE) ==
            RESONATE_PAL_OK);
    REQUIRE(file.handle != nullptr);

    const std::uint32_t written_values[4] = {11U, 22U, 33U, 44U};
    std::uint64_t written = 0;
    REQUIRE(resonate_pal_io_write(&file, written_values, sizeof(written_values), &written) ==
            RESONATE_PAL_OK);
    REQUIRE(written == sizeof(written_values));
    REQUIRE(resonate_pal_io_flush(&file) == RESONATE_PAL_OK);

    std::uint64_t size = 0;
    REQUIRE(resonate_pal_io_size(&file, &size) == RESONATE_PAL_OK);
    REQUIRE(size == sizeof(written_values));

    std::uint64_t position = 0;
    REQUIRE(resonate_pal_io_seek(&file, 0, RESONATE_SEEK_BEGIN, &position) == RESONATE_PAL_OK);
    REQUIRE(position == 0U);

    std::uint32_t read_values[4] = {};
    std::uint64_t read = 0;
    REQUIRE(resonate_pal_io_read(&file, read_values, sizeof(read_values), &read) ==
            RESONATE_PAL_OK);
    REQUIRE(read == sizeof(read_values));
    REQUIRE(read_values[2] == 33U);

    REQUIRE(resonate_pal_io_tell(&file, &position) == RESONATE_PAL_OK);
    REQUIRE(position == sizeof(written_values));

    resonate_pal_io_close_file(&file);
    REQUIRE(resonate_pal_io_exists(TEST_FILE) == 1U);
    REQUIRE(resonate_pal_io_is_directory(TEST_FILE) == 0U);

    REQUIRE(resonate_pal_io_rename(TEST_FILE, TEST_FILE_RENAMED) == RESONATE_PAL_OK);
    REQUIRE(resonate_pal_io_exists(TEST_FILE) == 0U);
    REQUIRE(resonate_pal_io_exists(TEST_FILE_RENAMED) == 1U);
    REQUIRE(resonate_pal_io_remove(TEST_FILE_RENAMED) == RESONATE_PAL_OK);
}

TEST_CASE("a missing file is reported as not found", "[pal][io]")
{
    ResonateFileHandle file = {};
    REQUIRE(resonate_pal_io_open_file(&file, "resonate_pal_absent_file.bin", RESONATE_FILE_READ) ==
            RESONATE_PAL_NOT_FOUND);
    REQUIRE(file.handle == nullptr);
    REQUIRE(resonate_pal_io_exists("resonate_pal_absent_file.bin") == 0U);
}

TEST_CASE("a directory can be enumerated", "[pal][io]")
{
    removeIfPresent(TEST_DIRECTORY);
    REQUIRE(resonate_pal_io_create_directory(TEST_DIRECTORY) == RESONATE_PAL_OK);
    REQUIRE(resonate_pal_io_is_directory(TEST_DIRECTORY) == 1U);

    /* One entry, so the enumeration has something to return before it ends. */
    const std::string entry_path = std::string(TEST_DIRECTORY) + "/entry.bin";
    ResonateFileHandle file = {};
    REQUIRE(resonate_pal_io_open_file(&file, entry_path.c_str(),
                                      RESONATE_FILE_WRITE | RESONATE_FILE_CREATE) ==
            RESONATE_PAL_OK);
    resonate_pal_io_close_file(&file);

    ResonateDirHandle directory = {};
    REQUIRE(resonate_pal_io_enumerate(&directory, TEST_DIRECTORY) == RESONATE_PAL_OK);

    const char* name = nullptr;
    REQUIRE(resonate_pal_io_enumerate_next(&directory, &name) == RESONATE_PAL_OK);
    REQUIRE(name != nullptr);
    REQUIRE(std::string(name) == "entry.bin");

    /* The dot entries are skipped, and the end is reported rather than signalled
       by a generic failure. */
    REQUIRE(resonate_pal_io_enumerate_next(&directory, &name) == RESONATE_PAL_EXHAUSTED);

    resonate_pal_io_close_directory(&directory);
    REQUIRE(resonate_pal_io_remove(entry_path.c_str()) == RESONATE_PAL_OK);
    REQUIRE(resonate_pal_io_remove(TEST_DIRECTORY) == RESONATE_PAL_OK);
}

TEST_CASE("the executable can locate itself", "[pal][io]")
{
    REQUIRE(resonate_pal_io_executable_path() != nullptr);
    REQUIRE(resonate_pal_io_executable_path()[0] != '\0');
    REQUIRE(resonate_pal_io_executable_directory() != nullptr);
    REQUIRE(resonate_pal_io_is_directory(resonate_pal_io_executable_directory()) == 1U);
}

TEST_CASE("mutual exclusion holds across threads", "[pal][sync][thread]")
{
    ResonateMutex mutex = {};
    REQUIRE(resonate_pal_sync_create_mutex(&mutex, 0) == RESONATE_PAL_OK);

    ResonateThreadHandle thread = {};
    std::atomic<int> counter{0};

    REQUIRE(resonate_pal_thread_create(
                &thread,
                [](void* argument)
                {
                    auto* state = static_cast<std::atomic<int>*>(argument);
                    if (resonate_pal_thread_is_main() == 0U)
                    {
                        state->fetch_add(1000);
                    }
                },
                &counter, 0, -1) == RESONATE_PAL_OK);

    REQUIRE(resonate_pal_thread_join(&thread) == RESONATE_PAL_OK);
    resonate_pal_thread_destroy(&thread);

    REQUIRE(counter.load() == 1000);
    REQUIRE(resonate_pal_thread_is_main() == 1U);
    REQUIRE(resonate_pal_sync_hardware_concurrency() >= 1U);

    resonate_pal_sync_lock_mutex(&mutex);
    resonate_pal_sync_unlock_mutex(&mutex);
    resonate_pal_sync_destroy_mutex(&mutex);
}

namespace
{

struct Handoff
{
    ResonateMutex* mutex = nullptr;
    ResonateConditionVariable condition = {};
    bool ready = false;
    std::atomic<int> counter{0};
};

void handOff(void* argument)
{
    auto* handoff = static_cast<Handoff*>(argument);
    handoff->counter.fetch_add(1);

    resonate_pal_sync_lock_mutex(handoff->mutex);
    handoff->ready = true;
    resonate_pal_sync_unlock_mutex(handoff->mutex);
    resonate_pal_sync_signal(&handoff->condition);
}

/* The wait has to release the mutex while it blocks: a caller that held it and a
   worker that then needs it is the deadlock this covers. */
void handOffRecursive(void* argument)
{
    auto* handoff = static_cast<Handoff*>(argument);

    resonate_pal_sync_lock_mutex(handoff->mutex);
    resonate_pal_sync_lock_mutex(handoff->mutex);
    handoff->counter.fetch_add(1);
    handoff->ready = true;
    resonate_pal_sync_unlock_mutex(handoff->mutex);
    resonate_pal_sync_unlock_mutex(handoff->mutex);
    resonate_pal_sync_signal(&handoff->condition);
}

void waitForHandoff(Handoff& handoff)
{
    resonate_pal_sync_lock_mutex(handoff.mutex);
    while (!handoff.ready)
    {
        resonate_pal_sync_wait(&handoff.condition, handoff.mutex);
    }
    resonate_pal_sync_unlock_mutex(handoff.mutex);
}

} // namespace

TEST_CASE("a condition variable hands work to a waiting thread", "[pal][sync][thread]")
{
    ResonateMutex mutex = {};
    REQUIRE(resonate_pal_sync_create_mutex(&mutex, 0) == RESONATE_PAL_OK);

    Handoff handoff = {};
    handoff.mutex = &mutex;
    REQUIRE(resonate_pal_sync_create_condition(&handoff.condition) == RESONATE_PAL_OK);

    ResonateThreadHandle thread = {};
    REQUIRE(resonate_pal_thread_create(&thread, &handOff, &handoff, 0, -1) == RESONATE_PAL_OK);

    waitForHandoff(handoff);
    REQUIRE(handoff.counter.load() == 1);

    REQUIRE(resonate_pal_thread_join(&thread) == RESONATE_PAL_OK);
    resonate_pal_thread_destroy(&thread);

    resonate_pal_sync_destroy_condition(&handoff.condition);
    resonate_pal_sync_destroy_mutex(&mutex);
}

TEST_CASE("a recursive mutex can be paired with a condition variable", "[pal][sync][thread]")
{
    ResonateMutex mutex = {};
    REQUIRE(resonate_pal_sync_create_mutex(&mutex, 1) == RESONATE_PAL_OK);

    Handoff handoff = {};
    handoff.mutex = &mutex;
    REQUIRE(resonate_pal_sync_create_condition(&handoff.condition) == RESONATE_PAL_OK);

    ResonateThreadHandle thread = {};
    REQUIRE(resonate_pal_thread_create(&thread, &handOffRecursive, &handoff, 0, -1) ==
            RESONATE_PAL_OK);

    waitForHandoff(handoff);
    REQUIRE(handoff.counter.load() == 1);

    REQUIRE(resonate_pal_thread_join(&thread) == RESONATE_PAL_OK);
    resonate_pal_thread_destroy(&thread);

    resonate_pal_sync_destroy_condition(&handoff.condition);
    resonate_pal_sync_destroy_mutex(&mutex);
}

namespace
{

/* Both of the tests below need a second thread: whether a primitive blocks is
   not observable from the thread holding it. */

constexpr uint64_t BLOCK_CHECK_NS = 50000000ULL;
constexpr uint64_t RELEASE_TIMEOUT_NS = 2000000000ULL;
constexpr uint64_t RELEASE_POLL_NS = 1000000ULL;

/* Waits, bounded, for a probe's flag to be set. A primitive that never releases
   fails the assertion instead of hanging the suite. */
void waitForFlag(const std::atomic<int32_t>& flag)
{
    uint64_t waited_ns = 0;
    while (flag.load() == 0 && waited_ns < RELEASE_TIMEOUT_NS)
    {
        resonate_pal_chrono_sleep(RELEASE_POLL_NS);
        waited_ns += RELEASE_POLL_NS;
    }
}

struct SemaphoreProbe
{
    ResonateSemaphore* semaphore = nullptr;
    std::atomic<int32_t> released{0};
};

void waitForSemaphore(void* argument)
{
    auto* probe = static_cast<SemaphoreProbe*>(argument);
    resonate_pal_sync_wait_semaphore(probe->semaphore);
    probe->released.store(1);
}

struct LockProbe
{
    ResonateReadWriteLock* lock = nullptr;
    std::atomic<int32_t> acquired{0};
};

void takeExclusive(void* argument)
{
    auto* probe = static_cast<LockProbe*>(argument);
    resonate_pal_sync_lock_exclusive(probe->lock);
    probe->acquired.store(1);
    resonate_pal_sync_unlock_read_write_lock(probe->lock);
}

} // namespace

TEST_CASE("a semaphore blocks while its count is zero", "[pal][sync][thread]")
{
    ResonateSemaphore semaphore = {};
    REQUIRE(resonate_pal_sync_create_semaphore(&semaphore, 0) == RESONATE_PAL_OK);

    SemaphoreProbe probe;
    probe.semaphore = &semaphore;

    ResonateThreadHandle thread = {};
    REQUIRE(resonate_pal_thread_create(&thread, &waitForSemaphore, &probe, 0, -1) ==
            RESONATE_PAL_OK);

    /* Nothing has signalled it, so a wait that actually blocks cannot have
       returned by now; one that is a no-op has. */
    resonate_pal_chrono_sleep(BLOCK_CHECK_NS);
    REQUIRE(probe.released.load() == 0);

    resonate_pal_sync_signal_semaphore(&semaphore, 1);
    waitForFlag(probe.released);
    REQUIRE(probe.released.load() == 1);

    REQUIRE(resonate_pal_thread_join(&thread) == RESONATE_PAL_OK);
    resonate_pal_sync_destroy_semaphore(&semaphore);
}

TEST_CASE("a read-write lock is exclusive against a shared holder", "[pal][sync][thread]")
{
    ResonateReadWriteLock lock = {};
    REQUIRE(resonate_pal_sync_create_read_write_lock(&lock) == RESONATE_PAL_OK);

    LockProbe probe;
    probe.lock = &lock;

    /* The main thread holds it shared, so an exclusive lock from another thread
       has to wait. This is the exclusion that a single thread cannot observe. */
    resonate_pal_sync_lock_shared(&lock);

    ResonateThreadHandle thread = {};
    REQUIRE(resonate_pal_thread_create(&thread, &takeExclusive, &probe, 0, -1) == RESONATE_PAL_OK);

    resonate_pal_chrono_sleep(BLOCK_CHECK_NS);
    REQUIRE(probe.acquired.load() == 0);

    resonate_pal_sync_unlock_read_write_lock(&lock);
    waitForFlag(probe.acquired);
    REQUIRE(probe.acquired.load() == 1);

    REQUIRE(resonate_pal_thread_join(&thread) == RESONATE_PAL_OK);
    resonate_pal_sync_destroy_read_write_lock(&lock);
}
