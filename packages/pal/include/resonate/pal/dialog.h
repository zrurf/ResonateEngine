#ifndef RESONATE_PAL_DIALOG_H
#define RESONATE_PAL_DIALOG_H

/*
 * Native file choosers. A desktop tool asks the platform for a path instead of
 * inventing one, so the file lands where the user expects.
 *
 * The choosers are modal and belong on the main thread. Backends answer
 * RESONATE_PAL_UNSUPPORTED when the platform has no chooser yet, which lets a
 * caller fall back to a path of its own rather than failing.
 */

#include <stdint.h>

#include "resonate/pal/status.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Shows the platform's save chooser. On OK, `out_selected` is 1 and `out_path`
   holds the chosen path as UTF-8 with a terminating NUL; 0 means the user
   cancelled. `default_name` may be a bare name or a full path that seeds the
   folder and the name. `extension` narrows the filter (for example "rscene").
   INVALID when a pointer is null or the chosen path does not fit `capacity`. */
ResonatePalStatus resonate_pal_save_dialog(const char* title, const char* default_name,
                                           const char* extension, char* out_path,
                                           uint32_t capacity, int32_t* out_selected);

/* The same for the open chooser: `extension` narrows the filter and the chosen
   file must exist. */
ResonatePalStatus resonate_pal_open_dialog(const char* title, const char* extension,
                                           char* out_path, uint32_t capacity,
                                           int32_t* out_selected);

#ifdef __cplusplus
}
#endif

#endif /* RESONATE_PAL_DIALOG_H */
