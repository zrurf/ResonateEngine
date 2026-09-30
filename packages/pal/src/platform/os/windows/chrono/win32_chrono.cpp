#include <resonate/pal/chrono.h>

#include <windows.h>

namespace
{

/* QueryPerformanceFrequency is fixed for the life of the process. */
uint64_t frequency()
{
    static const uint64_t value = []
    {
        LARGE_INTEGER counter = {};
        QueryPerformanceFrequency(&counter);
        return static_cast<uint64_t>(counter.QuadPart);
    }();
    return value;
}

} // namespace

extern "C"
{

uint64_t resonate_pal_chrono_ticks(void)
{
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

ResonateClockInfo resonate_pal_chrono_clock_info(void)
{
    const uint64_t rate = frequency();
    ResonateClockInfo info = {};
    info.frequency = rate;
    info.resolution_ns = rate != 0U ? (1000000000ULL + rate - 1ULL) / rate : 0ULL;
    return info;
}

/* Split the division so a long uptime cannot overflow the intermediate. */
uint64_t resonate_pal_chrono_ticks_to_nanoseconds(uint64_t ticks)
{
    const uint64_t rate = frequency();
    if (rate == 0U)
    {
        return 0;
    }
    return (ticks / rate) * 1000000000ULL + ((ticks % rate) * 1000000000ULL) / rate;
}

uint64_t resonate_pal_chrono_nanoseconds_to_ticks(uint64_t nanoseconds)
{
    const uint64_t rate = frequency();
    return (nanoseconds / 1000000000ULL) * rate +
           ((nanoseconds % 1000000000ULL) * rate) / 1000000000ULL;
}

ResonateTimestamp resonate_pal_chrono_now(void)
{
    ResonateTimestamp timestamp = {};
    timestamp.nanoseconds = resonate_pal_chrono_ticks_to_nanoseconds(resonate_pal_chrono_ticks());
    return timestamp;
}

/* Rounded up to the millisecond: the caller is promised at least the requested
   duration. */
void resonate_pal_chrono_sleep(uint64_t nanoseconds)
{
    const uint64_t milliseconds = (nanoseconds + 999999ULL) / 1000000ULL;
    if (milliseconds == 0ULL)
    {
        SwitchToThread();
        return;
    }
    Sleep(static_cast<DWORD>(milliseconds));
}

} // extern "C"
