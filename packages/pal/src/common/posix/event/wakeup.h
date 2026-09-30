#ifndef RESONATE_PAL_POSIX_EVENT_WAKEUP_H
#define RESONATE_PAL_POSIX_EVENT_WAKEUP_H

#include <cstdint>

namespace resonate::pal::posix
{

/*
 * The one part of the POSIX event loop that is not shared: a waitable descriptor
 * the workers signal when a completion is ready. eventfd where the kernel has it,
 * a pipe otherwise.
 *
 * Both ends travel together because a pipe is signalled by writing to the end it
 * does not wait on. For eventfd they are the same descriptor.
 */
struct Wakeup
{
    int wait = -1;
    int signal = -1;
};

Wakeup wakeupCreate();
void wakeupDestroy(Wakeup& wakeup);

void wakeupSignal(const Wakeup& wakeup);

/* Reads the descriptor back to its unsignalled state. */
void wakeupDrain(const Wakeup& wakeup);

/* Outcome of a wait. A handled signal is its own case rather than a timeout: the
   wait was cut short rather than found wanting, and a caller with a deadline to
   keep waits again for what is left of it. */
enum WakeupResult
{
    WAKEUP_SIGNALLED = 0,
    WAKEUP_EXPIRED = 1,
    WAKEUP_INTERRUPTED = 2,
    WAKEUP_FAILED = -1
};

int wakeupWait(const Wakeup& wakeup, uint64_t timeout_nanoseconds);

} // namespace resonate::pal::posix

#endif /* RESONATE_PAL_POSIX_EVENT_WAKEUP_H */
