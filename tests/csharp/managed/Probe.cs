using System;
using System.Runtime.InteropServices;
using Resonate.CSharp.Bindings;

namespace Resonate.Tests.Managed;

/* The native side hands this struct to Register; it mirrors
   packages/csharp/include/resonate/csharp/registration.h field for field. */
[StructLayout(LayoutKind.Sequential)]
public unsafe struct Registration
{
    public IntPtr Context;
    public delegate* unmanaged<IntPtr, uint, IntPtr, int> RegisterEntry;
}

/* The native host test reaches these through their unmanaged entry points. */
public static unsafe class Probe
{
    [UnmanagedCallersOnly]
    public static int Add(int left, int right)
    {
        return left + right;
    }

    /* Registration path: the managed assembly places its own entry in the
       engine's table, so the engine never holds a delegate. */
    [UnmanagedCallersOnly]
    public static int Register(IntPtr api)
    {
        Registration* registration = (Registration*)api;
        return registration->RegisterEntry(registration->Context, Systems.Tick,
                                           (IntPtr)(delegate* unmanaged<IntPtr, void>)&Tick);
    }

    /* The registered entry: it writes through the native context pointer, the
       zero-marshal shape the frame path uses. */
    [UnmanagedCallersOnly]
    public static void Tick(IntPtr context)
    {
        *(int*)context += 1;
    }
}
