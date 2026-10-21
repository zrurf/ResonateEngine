using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Resonate.Tests.Bootstrap;

/* What the engine asks for: the assembly to load, the type holding its
   registration method, the registration the patch should use, and the handle
   the host writes back for a later unload. */
[StructLayout(LayoutKind.Sequential)]
public unsafe struct InstallRequest
{
    public IntPtr Path;
    public IntPtr EntryType;
    public IntPtr Registration;
    public IntPtr Context;
}

/* Loads a patch assembly into its own collectible AssemblyLoadContext and lets
   it replace dispatch slots. The engine keeps only function pointers; this is
   the one place that knows about managed assembly loading, which is what makes
   the old patch collectible after the swap. */
public static unsafe class PatchHost
{
    /* Host infrastructure, not game state: the behaviour law does not apply to
       this assembly. */
    private static readonly Dictionary<int, AssemblyLoadContext> Contexts = new();
    private static int NextHandle = 1;

    [UnmanagedCallersOnly]
    public static int Install(IntPtr request)
    {
        try
        {
            InstallRequest* install = (InstallRequest*)request;
            string? path = Marshal.PtrToStringUTF8(install->Path);
            string? entry = Marshal.PtrToStringUTF8(install->EntryType);
            if (path is null || entry is null)
            {
                return 1;
            }

            AssemblyLoadContext context = new($"resonate-patch-{NextHandle}", isCollectible: true);
            Assembly assembly = context.LoadFromAssemblyPath(path);
            Type? type = assembly.GetType(entry);
            MethodInfo? register = type?.GetMethod("Register",
                                                   BindingFlags.Public | BindingFlags.Static);
            if (register is null)
            {
                context.Unload();
                return 2;
            }

            object? status = register.Invoke(null, new object[] { install->Registration });
            if (status is not int code || code != 0)
            {
                context.Unload();
                return 3;
            }

            int handle = NextHandle++;
            Contexts[handle] = context;
            install->Context = new IntPtr(handle);
            return 0;
        }
        catch (Exception)
        {
            return 4;
        }
    }

    /* The slot must already have been replaced away from the patch before this:
       the table holds raw function pointers into code the unload frees. */
    [UnmanagedCallersOnly]
    public static int Unload(IntPtr handle)
    {
        try
        {
            if (!Contexts.Remove(handle.ToInt32(), out AssemblyLoadContext? context))
            {
                return 1;
            }
            context.Unload();
            return 0;
        }
        catch (Exception)
        {
            return 2;
        }
    }
}
