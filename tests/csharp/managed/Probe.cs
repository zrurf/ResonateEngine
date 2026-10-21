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

    /* Reads a generated struct field by field: the engine lays the frame view
       out, the managed side only reads it. */
    [UnmanagedCallersOnly]
    public static float SumTransforms(IntPtr view)
    {
        FrameView* frame = (FrameView*)view;
        ReadOnlySpan<float> transforms = new(frame->TransformData, (int)frame->TransformCount);
        float sum = 0.0F;
        for (int index = 0; index < transforms.Length; ++index)
        {
            sum += transforms[index];
        }
        return sum;
    }

    /* Reads the typed handle wrapper; a raw integer would be indistinguishable
       from any other id at the boundary. */
    [UnmanagedCallersOnly]
    public static ulong ReadHandle(IntPtr view)
    {
        FrameView* frame = (FrameView*)view;
        return ((ulong)frame->Handle.Generation << 32) | frame->Handle.Index;
    }

    /* Runtime check that the two compilers agree with the generated layout; a
       mismatch means the boundary struct is unsafe to share. */
    [UnmanagedCallersOnly]
    public static int VerifyFrameViewLayout()
    {
        if (Marshal.SizeOf<FrameView>() != FrameViewLayout.Size ||
            Marshal.OffsetOf<FrameView>(nameof(FrameView.Handle)) !=
                FrameViewLayout.HandleOffset ||
            Marshal.OffsetOf<FrameView>(nameof(FrameView.DeltaSeconds)) !=
                FrameViewLayout.DeltaSecondsOffset ||
            Marshal.OffsetOf<FrameView>(nameof(FrameView.FrameIndex)) !=
                FrameViewLayout.FrameIndexOffset ||
            Marshal.OffsetOf<FrameView>(nameof(FrameView.TransformData)) !=
                FrameViewLayout.TransformDataOffset ||
            Marshal.OffsetOf<FrameView>(nameof(FrameView.TransformCount)) !=
                FrameViewLayout.TransformCountOffset)
        {
            return 1;
        }
        return 0;
    }

    /* The schema-generated component views: the managed side reads the same
       chunk layout the C++ side writes. */
    [UnmanagedCallersOnly]
    public static int VerifySchemaLayouts()
    {
        return Resonate.Render.LayoutChecks.Verify() &&
               Resonate.Gameplay.LayoutChecks.Verify() &&
               Resonate.Hierarchy.LayoutChecks.Verify()
            ? 0
            : 1;
    }

    [UnmanagedCallersOnly]
    public static float SumFarZ(IntPtr cameras, int count)
    {
        ReadOnlySpan<Resonate.Render.Camera> view =
            new((Resonate.Render.Camera*)cameras, count);
        float sum = 0.0F;
        for (int index = 0; index < view.Length; ++index)
        {
            sum += view[index].FarZ;
        }
        return sum;
    }
}
