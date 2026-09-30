#ifndef RESONATE_PAL_POSIX_STATUS_H
#define RESONATE_PAL_POSIX_STATUS_H

#include "resonate/pal/status.h"

namespace resonate::pal::posix
{

/* The POSIX counterpart of the Win32 mapping: every backend funnels errno
   through here so no raw error number reaches a caller. */
ResonatePalStatus statusFromErrno(int error);

/* statusFromErrno(errno). */
ResonatePalStatus statusFromLastErrno();

} // namespace resonate::pal::posix

#endif /* RESONATE_PAL_POSIX_STATUS_H */
