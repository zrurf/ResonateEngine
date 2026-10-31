#include <resonate/pal/dialog.h>

#include <cstdio>
#include <iterator>
#include <string>

#include <windows.h>
#include <commdlg.h>

namespace
{

/* The choosers take UTF-16 and a double-NUL-terminated filter list; the rest
   of the API is UTF-8, so both conversions happen here. */
bool toWide(const char* utf8, wchar_t* out, std::size_t capacity)
{
    if (utf8 == nullptr)
    {
        out[0] = L'\0';
        return true;
    }
    const int written =
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, static_cast<int>(capacity));
    return written > 0;
}

bool toUtf8(const wchar_t* wide, char* out, std::size_t capacity)
{
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out,
                                            static_cast<int>(capacity), nullptr, nullptr);
    return written > 0;
}

/* "Documents (*.rscene)" then "*.rscene" then the catch-all, each entry and the
   list itself closed by a NUL. */
void buildFilter(const char* extension, std::wstring& out)
{
    wchar_t pattern[64] = {};
    wchar_t suffix[64] = {};
    if (extension != nullptr && extension[0] != '\0' &&
        toWide(extension, suffix, std::size(suffix)))
    {
        std::swprintf(pattern, std::size(pattern), L"*.%ls", suffix);
    }
    else
    {
        std::swprintf(pattern, std::size(pattern), L"*.*");
    }
    out = L"Documents (";
    out += pattern;
    out += L")";
    out.push_back(L'\0');
    out += pattern;
    out.push_back(L'\0');
    out += L"All files";
    out.push_back(L'\0');
    out += L"*.*";
    out.push_back(L'\0');
    out.push_back(L'\0');
}

ResonatePalStatus showDialog(bool save, const char* title, const char* default_name,
                             const char* extension, char* out_path, uint32_t capacity,
                             int32_t* out_selected)
{
    if (out_path == nullptr || out_selected == nullptr || capacity == 0U)
    {
        return RESONATE_PAL_INVALID;
    }
    wchar_t file[1024] = {};
    if (!toWide(default_name, file, std::size(file)))
    {
        return RESONATE_PAL_INVALID;
    }
    std::wstring filter;
    buildFilter(extension, filter);
    wchar_t heading[256] = {};
    if (!toWide(title, heading, std::size(heading)))
    {
        return RESONATE_PAL_INVALID;
    }

    OPENFILENAMEW options = {};
    options.lStructSize = sizeof(options);
    options.hwndOwner = nullptr;
    options.lpstrFilter = filter.c_str();
    options.lpstrFile = file;
    options.nMaxFile = static_cast<DWORD>(std::size(file));
    options.lpstrTitle = heading;
    options.Flags =
        OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

    const BOOL chosen = save ? GetSaveFileNameW(&options) : GetOpenFileNameW(&options);
    if (chosen == FALSE)
    {
        /* A cancel is an answer, not a failure. */
        const DWORD error = CommDlgExtendedError();
        *out_selected = 0;
        return error == 0U ? RESONATE_PAL_OK : RESONATE_PAL_IO;
    }
    if (!toUtf8(file, out_path, capacity))
    {
        return RESONATE_PAL_INVALID;
    }
    *out_selected = 1;
    return RESONATE_PAL_OK;
}

} // namespace

extern "C"
{

ResonatePalStatus resonate_pal_save_dialog(const char* title, const char* default_name,
                                           const char* extension, char* out_path,
                                           uint32_t capacity, int32_t* out_selected)
{
    return showDialog(true, title, default_name, extension, out_path, capacity, out_selected);
}

ResonatePalStatus resonate_pal_open_dialog(const char* title, const char* extension,
                                           char* out_path, uint32_t capacity, int32_t* out_selected)
{
    return showDialog(false, title, nullptr, extension, out_path, capacity, out_selected);
}

} // extern "C"
