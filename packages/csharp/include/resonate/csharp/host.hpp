#ifndef RESONATE_CSHARP_HOST_H
#define RESONATE_CSHARP_HOST_H

#include <string>

namespace resonate::csharp
{

/* One CoreCLR per process, hosted without linking anything from the .NET SDK:
   the runtime's own hostfxr is located next to the installed runtime at run
   time. A component is an assembly whose runtimeconfig names the framework it
   expects; functions are reached through [UnmanagedCallersOnly] entry points,
   so no delegate marshaling happens on the hot path. */
class RuntimeHost
{
public:
    RuntimeHost() = default;
    ~RuntimeHost();

    RuntimeHost(const RuntimeHost&) = delete;
    RuntimeHost& operator=(const RuntimeHost&) = delete;

    /* Starts the runtime from the component's *.runtimeconfig.json. */
    bool initialize(const std::string& runtime_config, std::string& out_error);

    /* Resolves an [UnmanagedCallersOnly] static method to its function pointer.
       `type_name` is the assembly-qualified name, e.g.
       "Namespace.Type, AssemblyName". */
    void* load_function(const std::string& assembly, const std::string& type_name,
                        const std::string& method, std::string& out_error);

    /* Closes the hosting context and releases the library handle. CoreCLR has
       no unload; a later initialize joins the runtime already running. */
    void shutdown();

private:
    void* library_ = nullptr;
    void* context_ = nullptr;
    void* loader_ = nullptr;
    void* close_ = nullptr;
};

} // namespace resonate::csharp

#endif /* RESONATE_CSHARP_HOST_H */
