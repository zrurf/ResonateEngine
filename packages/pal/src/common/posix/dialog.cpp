#include <resonate/pal/dialog.h>

/*
 * No chooser on this backend yet. A desktop Linux or macOS build needs a
 * platform dialog (GTK, zenity, NSOpenPanel); until then the caller is told so
 * and can fall back to a path of its own.
 */
extern "C"
{

ResonatePalStatus resonate_pal_save_dialog(const char* title, const char* default_name,
                                           const char* extension, char* out_path,
                                           uint32_t capacity, int32_t* out_selected)
{
    (void)title;
    (void)default_name;
    (void)extension;
    if (out_path == nullptr || out_selected == nullptr || capacity == 0U)
    {
        return RESONATE_PAL_INVALID;
    }
    *out_selected = 0;
    return RESONATE_PAL_UNSUPPORTED;
}

ResonatePalStatus resonate_pal_open_dialog(const char* title, const char* extension,
                                           char* out_path, uint32_t capacity, int32_t* out_selected)
{
    (void)title;
    (void)extension;
    if (out_path == nullptr || out_selected == nullptr || capacity == 0U)
    {
        return RESONATE_PAL_INVALID;
    }
    *out_selected = 0;
    return RESONATE_PAL_UNSUPPORTED;
}

} // extern "C"
