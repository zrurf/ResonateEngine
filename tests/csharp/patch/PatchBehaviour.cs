using System;
using System.Runtime.InteropServices;
using Resonate.CSharp.Bindings;

namespace Resonate.Tests.Patch;

/* A patch assembly: a normal (reflection-invokable) Register places its own
   unmanaged entry into the engine's table through the registration it is
   handed, so the same code serves the native loader and an ALC swap. */
public static unsafe class PatchBehaviour
{
    public static int Register(IntPtr api)
    {
        Registration* registration = (Registration*)api;
        return registration->RegisterEntry(registration->Context, Systems.Tick,
                                           (IntPtr)(delegate* unmanaged<IntPtr, void>)&Tick);
    }

    /* The patched entry: it reports itself through the native counters. */
    [UnmanagedCallersOnly]
    public static void Tick(IntPtr context)
    {
        SystemContext* frame = (SystemContext*)context;
        int* counters = (int*)frame->User;
        counters[0] += 1;
        counters[1] = 2;
    }
}
