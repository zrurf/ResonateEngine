#include <resonate/pal/chrono.h>

#include <cerrno>
#include <ctime>

namespace
{

/* CLOCK_MONOTONIC counts nanoseconds: a tick is a nanosecond. */
uint64_t nowNanoseconds()
{
    timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0;
    }
    return (static_cast<uint64_t>(now.tv_sec) * 1000000000ULL) + static_cast<uint64_t>(now.tv_nsec);
}

} // namespace

extern "C"
{

uint64_t resonate_pal_chrono_ticks(void)
{
    return nowNanoseconds();
}

ResonateClockInfo resonate_pal_chrono_clock_info(void)
{
    ResonateClockInfo info = {};
    info.frequency = 1000000000ULL;
    info.resolution_ns = 1;
    return info;
}

uint64_t resonate_pal_chrono_ticks_to_nanoseconds(uint64_t ticks)
{
    return ticks;
}

uint64_t resonate_pal_chrono_nanoseconds_to_ticks(uint64_t nanoseconds)
{
    return nanoseconds;
}

ResonateTimestamp resonate_pal_chrono_now(void)
{
    ResonateTimestamp timestamp = {};
    timestamp.nanoseconds = nowNanoseconds();
    return timestamp;
}

/* Retried on EINTR so the sleep lasts the requested duration. */
void resonate_pal_chrono_sleep(uint64_t nanoseconds)
{
    timespec remaining = {};
    remaining.tv_sec = static_cast<time_t>(nanoseconds / 1000000000ULL);
    remaining.tv_nsec = static_cast<long>(nanoseconds % 1000000000ULL);

    timespec* request = &remaining;
    while (nanosleep(request, request) != 0 && errno == EINTR)
    {
    }
}

} // extern "C"
