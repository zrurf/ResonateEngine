#ifndef RESONATE_PAL_WINDOWS_STATUS_H
#define RESONATE_PAL_WINDOWS_STATUS_H

#include "resonate/pal/status.h"

namespace resonate::pal::windows
{

/* Maps a GetLastError() code to the PAL table; every Win32 failure in this
   backend goes through it. */
ResonatePalStatus statusFromWin32(unsigned long error);

/* statusFromWin32(GetLastError()), for the common case. */
ResonatePalStatus statusFromLastError();

} // namespace resonate::pal::windows

#endif /* RESONATE_PAL_WINDOWS_STATUS_H */
