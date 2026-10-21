#include <resonate/csharp/host.hpp>

#include <resonate/pal/library.h>
#include <resonate/pal/status.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace resonate::csharp
{
namespace
{

/* The hostfxr ABI, declared here instead of including the SDK's headers: the
   build must not depend on where an SDK happens to be installed, and this
   surface is stable. hostfxr.h and coreclr_delegates.h document it; the string
   type is wchar_t on Windows and char elsewhere, and the calling convention is
   the platform default on the 64-bit targets this engine supports. */
#ifdef RESONATE_PLATFORM_WINDOWS
using HostChar = wchar_t;
#else
using HostChar = char;
#endif

using HostfxrHandle = void*;
using InitializeFn = std::int32_t (*)(const HostChar* runtime_config, const void* parameters,
                                      HostfxrHandle* context);
using GetDelegateFn = std::int32_t (*)(HostfxrHandle context, std::int32_t type, void** out);
using CloseFn = std::int32_t (*)(HostfxrHandle context);
using LoadFunctionFn = std::int32_t (*)(const HostChar* assembly, const HostChar* type_name,
                                        const HostChar* method, const HostChar* delegate_type,
                                        void* reserved, void** out);

constexpr std::int32_t kLoadAssemblyAndGetFunctionPointer = 5;

const HostChar* unmanagedCallersOnly()
{
    return reinterpret_cast<const HostChar*>(-1);
}

std::basic_string<HostChar> nativePath(const std::string& utf8)
{
#ifdef RESONATE_PLATFORM_WINDOWS
    /* u8path carries a deprecation warning in C++20; the char8_t path
       constructor is the same conversion. */
    const std::u8string text(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(text).wstring();
#else
    return utf8;
#endif
}

std::string dotnetRoot()
{
    if (const char* root = std::getenv("DOTNET_ROOT"); root != nullptr && *root != '\0')
    {
        return root;
    }
#ifdef RESONATE_PLATFORM_WINDOWS
    if (const char* program_files = std::getenv("ProgramFiles"); program_files != nullptr)
    {
        const std::string candidate = std::string(program_files) + "/dotnet";
        if (std::filesystem::is_directory(candidate))
        {
            return candidate;
        }
    }
#else
    for (const char* candidate : {"/usr/share/dotnet", "/usr/local/share/dotnet", "/usr/lib/dotnet"})
    {
        if (std::filesystem::is_directory(candidate))
        {
            return candidate;
        }
    }
#endif
    return {};
}

const char* hostfxrLibraryName()
{
#if defined(RESONATE_PLATFORM_WINDOWS)
    return "hostfxr.dll";
#elif defined(RESONATE_PLATFORM_MACOS)
    return "libhostfxr.dylib";
#else
    return "libhostfxr.so";
#endif
}

bool parseVersion(const std::string& name, int out[3])
{
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    int part = 0;
    std::size_t index = 0;
    while (index < name.size() && part < 3)
    {
        if (name[index] < '0' || name[index] > '9')
        {
            break;
        }
        int value = 0;
        while (index < name.size() && name[index] >= '0' && name[index] <= '9')
        {
            value = (value * 10) + (name[index] - '0');
            ++index;
        }
        out[part++] = value;
        if (index < name.size() && name[index] == '.')
        {
            ++index;
        }
    }
    return part > 0;
}

bool newer(const int left[3], const int right[3])
{
    for (int part = 0; part < 3; ++part)
    {
        if (left[part] != right[part])
        {
            return left[part] > right[part];
        }
    }
    return false;
}

/* The runtime stores hostfxr under host/fxr/<version>; a prerelease suffix is
   ignored, so 11.0.0-rc.1 orders above 10.0.12. */
std::string hostfxrPath(const std::string& root)
{
    const std::filesystem::path directory = std::filesystem::path(root) / "host" / "fxr";
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error))
    {
        return {};
    }

    std::string best_name;
    int best_version[3] = {-1, -1, -1};
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, error))
    {
        if (!entry.is_directory(error))
        {
            continue;
        }
        const std::string name = entry.path().filename().string();
        int version[3] = {};
        if (!parseVersion(name, version))
        {
            continue;
        }
        if (best_name.empty() || newer(version, best_version))
        {
            best_name = name;
            best_version[0] = version[0];
            best_version[1] = version[1];
            best_version[2] = version[2];
        }
    }
    if (best_name.empty())
    {
        return {};
    }
    return (directory / best_name / hostfxrLibraryName()).string();
}

/* hostfxr stays loaded for the process: CoreCLR cannot unload, and a hostfxr
   loaded again after the runtime started refuses to initialize. Every host
   shares this one handle. */
void* hostfxrLibrary(std::string& out_error)
{
    static void* library = nullptr;
    static bool attempted = false;
    static std::string failure;

    if (!attempted)
    {
        attempted = true;
        const std::string root = dotnetRoot();
        if (root.empty())
        {
            failure = "no .NET runtime root found (set DOTNET_ROOT)";
        }
        else
        {
            const std::string path = hostfxrPath(root);
            if (path.empty())
            {
                failure = "no hostfxr under " + root + "/host/fxr";
            }
            else
            {
                ResonateLibrary loaded{};
                if (resonate_pal_library_load(&loaded, path.c_str()) != RESONATE_PAL_OK)
                {
                    failure = "loading " + path + " failed: " + resonate_pal_library_error();
                }
                else
                {
                    library = loaded.handle;
                }
            }
        }
    }

    if (library == nullptr)
    {
        out_error = failure;
    }
    return library;
}

} // namespace

RuntimeHost::~RuntimeHost()
{
    shutdown();
}

bool RuntimeHost::initialize(const std::string& runtime_config, std::string& out_error)
{
    out_error.clear();
    if (loader_ != nullptr)
    {
        return true;
    }

    void* library = hostfxrLibrary(out_error);
    if (library == nullptr)
    {
        return false;
    }
    ResonateLibrary handle{library};
    const auto initialize_fn = reinterpret_cast<InitializeFn>(
        resonate_pal_library_symbol(&handle, "hostfxr_initialize_for_runtime_config"));
    const auto get_delegate_fn = reinterpret_cast<GetDelegateFn>(
        resonate_pal_library_symbol(&handle, "hostfxr_get_runtime_delegate"));
    const auto close_fn = reinterpret_cast<CloseFn>(
        resonate_pal_library_symbol(&handle, "hostfxr_close"));
    if (initialize_fn == nullptr || get_delegate_fn == nullptr || close_fn == nullptr)
    {
        out_error = "hostfxr is missing the hosting entry points";
        return false;
    }

    const std::basic_string<HostChar> config = nativePath(runtime_config);
    HostfxrHandle context = nullptr;
    const std::int32_t status = initialize_fn(config.c_str(), nullptr, &context);
    if (status < 0 || context == nullptr)
    {
        out_error = "hostfxr initialization failed (" + std::to_string(status) + ")";
        return false;
    }

    void* loader = nullptr;
    const std::int32_t delegate_status =
        get_delegate_fn(context, kLoadAssemblyAndGetFunctionPointer, &loader);
    if (delegate_status < 0 || loader == nullptr)
    {
        close_fn(context);
        out_error = "hostfxr refused the assembly loader (" + std::to_string(delegate_status) + ")";
        return false;
    }

    library_ = library;
    context_ = context;
    loader_ = loader;
    close_ = reinterpret_cast<void*>(close_fn);
    return true;
}

void* RuntimeHost::load_function(const std::string& assembly, const std::string& type_name,
                                 const std::string& method, std::string& out_error)
{
    out_error.clear();
    if (loader_ == nullptr)
    {
        out_error = "the runtime is not initialized";
        return nullptr;
    }

    const auto load = reinterpret_cast<LoadFunctionFn>(loader_);
    const std::basic_string<HostChar> native_assembly = nativePath(assembly);
    const std::basic_string<HostChar> native_type = nativePath(type_name);
    const std::basic_string<HostChar> native_method = nativePath(method);
    void* function = nullptr;
    const std::int32_t status =
        load(native_assembly.c_str(), native_type.c_str(), native_method.c_str(),
             unmanagedCallersOnly(), nullptr, &function);
    if (status != 0 || function == nullptr)
    {
        out_error = "resolving " + type_name + "." + method + " failed (" + std::to_string(status) +
                    ")";
        return nullptr;
    }
    return function;
}

void RuntimeHost::shutdown()
{
    loader_ = nullptr;
    if (context_ != nullptr && close_ != nullptr)
    {
        reinterpret_cast<CloseFn>(close_)(context_);
    }
    context_ = nullptr;
    close_ = nullptr;
    library_ = nullptr;
}

} // namespace resonate::csharp
