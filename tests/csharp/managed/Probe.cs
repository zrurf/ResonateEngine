using System;
using System.Runtime.InteropServices;
using Resonate.CSharp.Bindings;

namespace Resonate.Tests.Managed;

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
                                           (IntPtr)(delegate* unmanaged<IntPtr, void>)&TickFrame);
    }

    /* The registered entry: the generated frame context arrives as a pointer,
       and the native state hangs off its user field. */
    [UnmanagedCallersOnly]
    public static void TickFrame(IntPtr context)
    {
        SystemContext* frame = (SystemContext*)context;
        int* counters = (int*)frame->User;
        counters[0] += 1;
        counters[1] = (int)frame->FrameIndex;

        HostApi* api = (HostApi*)frame->HostApi;
        if (frame->FrameIndex == 0 && counters[6] == 1)
        {
            ulong entity = 0;
            int status = api->Spawn((IntPtr)api, &entity);
            if (status == 0)
            {
                Resonate.Hierarchy.LocalTransform transform = default;
                transform.Position = new Resonate.CSharp.Math.Vec3
                {
                    X = 4.0F,
                    Y = 5.0F,
                    Z = 6.0F,
                };
                transform.Scale = new Resonate.CSharp.Math.Vec3
                {
                    X = 1.0F,
                    Y = 1.0F,
                    Z = 1.0F,
                };
                fixed (byte* name = Resonate.Hierarchy.LocalTransform.SchemaNameUtf8)
                {
                    status = api->AddComponent((IntPtr)api, entity, name, (IntPtr)(&transform));
                }
            }
            counters[6] = status;
            counters[7] = (int)entity;
        }

        if (frame->ChunkData == IntPtr.Zero || frame->ChunkCount == 0)
        {
            counters[2] = -1;
            return;
        }
        Span<Resonate.Hierarchy.LocalTransform> transforms =
            new((Resonate.Hierarchy.LocalTransform*)frame->ChunkData, (int)frame->ChunkCount);
        float sum = 0.0F;
        for (int index = 0; index < transforms.Length; ++index)
        {
            sum += transforms[index].Position.X;
            transforms[index].Position.Y = 1.0F;
        }
        counters[2] += (int)(sum * 10.0F);
        counters[3] += (int)frame->ChunkCount;
        EntityHandle* entities = (EntityHandle*)frame->EntityData;
        counters[4] = (int)entities[0].Index;

        counters[5] = api->MarkChunkChanged((IntPtr)api);
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
