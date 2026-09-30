#include "wakeup.h"

#include <cerrno>
#include <cstdint>
#include <unistd.h>

#include <fcntl.h>
#include <poll.h>

#if defined(__linux__) || defined(__ANDROID__) || defined(__OHOS__)
#    include <sys/eventfd.h>
#endif

namespace resonate::pal::posix
{
namespace
{

int millisecondsFrom(uint64_t nanoseconds)
{
    if (nanoseconds == 0U)
    {
        return 0;
    }
    const uint64_t milliseconds = nanoseconds / 1000000ULL;
    if (milliseconds == 0ULL)
    {
        return 1;
    }
    return milliseconds > 0x7FFFFFFFULL ? -1 : static_cast<int>(milliseconds);
}

} // namespace

#if defined(__linux__) || defined(__ANDROID__) || defined(__OHOS__)

Wakeup wakeupCreate()
{
    Wakeup wakeup;
    wakeup.wait = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    wakeup.signal = wakeup.wait;
    return wakeup;
}

/* Any amount in the counter means signalled, so this drains until it is empty. */
void wakeupDrain(const Wakeup& wakeup)
{
    uint64_t value = 0;
    while (read(wakeup.wait, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)))
    {
    }
}

#else

Wakeup wakeupCreate()
{
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0)
    {
        return Wakeup{};
    }

    for (int index = 0; index < 2; ++index)
    {
        fcntl(descriptors[index], F_SETFL, O_NONBLOCK);
        fcntl(descriptors[index], F_SETFD, FD_CLOEXEC);
    }

    /* Wait on the read end, signal by writing to the other one. */
    return Wakeup{descriptors[0], descriptors[1]};
}

void wakeupDrain(const Wakeup& wakeup)
{
    uint8_t value = 0;
    while (read(wakeup.wait, &value, sizeof(value)) > 0)
    {
    }
}

#endif

void wakeupDestroy(Wakeup& wakeup)
{
    if (wakeup.wait >= 0)
    {
        close(wakeup.wait);
    }
    /* eventfd hands out one descriptor twice, so only a distinct second one is
       closed here. */
    if (wakeup.signal >= 0 && wakeup.signal != wakeup.wait)
    {
        close(wakeup.signal);
    }
    wakeup = Wakeup{};
}

void wakeupSignal(const Wakeup& wakeup)
{
    if (wakeup.signal < 0)
    {
        return;
    }

    /* A pipe that is already full is already readable, so a dropped write costs
       nothing. */
    const uint64_t value = 1;
    const ssize_t written = write(wakeup.signal, &value, sizeof(value));
    (void)written;
}

int wakeupWait(const Wakeup& wakeup, uint64_t timeout_nanoseconds)
{
    if (wakeup.wait < 0)
    {
        return WAKEUP_FAILED;
    }

    pollfd entry = {};
    entry.fd = wakeup.wait;
    entry.events = POLLIN;

    const int result = poll(&entry, 1, millisecondsFrom(timeout_nanoseconds));
    if (result < 0)
    {
        return errno == EINTR ? WAKEUP_INTERRUPTED : WAKEUP_FAILED;
    }
    return result == 0 ? WAKEUP_EXPIRED : WAKEUP_SIGNALLED;
}

} // namespace resonate::pal::posix
