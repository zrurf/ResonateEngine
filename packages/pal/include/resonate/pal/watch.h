#ifndef RESONATE_PAL_WATCH_H
#define RESONATE_PAL_WATCH_H

/*
 * Change notification for one file. The backend watches the file's directory,
 * so an editor that replaces the file by rename keeps working (a watch on the
 * inode would go stale); a notification may therefore name a sibling, and the
 * caller compares its own timestamps to decide whether its file changed.
 * Polling never blocks, so the run checks once per frame.
 */

#include <stdint.h>

#include "resonate/pal/status.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateWatchHandle
{
    void* handle;
} ResonateWatchHandle;

/* `file_path` names the file to watch; its directory must exist. */
ResonatePalStatus resonate_pal_watch_create(ResonateWatchHandle* out, const char* file_path);

/* 1 when the directory changed since the previous poll, 0 when nothing
   happened, and -1 when the notification could not be read. A transient
   failure keeps the watch; a permanent one keeps answering until it is
   closed. */
int32_t resonate_pal_watch_poll(ResonateWatchHandle* watch);

void resonate_pal_watch_close(ResonateWatchHandle* watch);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_WATCH_H */
