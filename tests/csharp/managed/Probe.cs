using System;
using System.Runtime.InteropServices;
using Resonate.CSharp;
using Resonate.CSharp.Bindings;

[assembly: ResonateBehaviour]

namespace Resonate.Tests.Managed;

/* The native host test reaches these through their unmanaged entry points. */
public static unsafe class Probe
{
    [UnmanagedCallersOnly]
    public static int Add(int left, int right)
    {
        return left + right;
    }

    /* The registered entry: the generated frame context arrives as a pointer,
       and the native state hangs off its user field. The registration
       generator places it in the UPDATE table.

       Counter words: 0 calls, 1 last frame index, 2 ΣX×10 (or -1 when there is
       no chunk), 3 rows, 4 first entity index, 5 self view disposed (its chunk
       mark went out), 6 spawn status,
       7 spawned index, 8 request on frame 1 (0 none, 1 remove, 2 despawn,
       3 read), 9 request result, 10 spawned generation, 11 wrong-type self
       view rejected. */
    [UpdateIn(ResonateStage.Update)]
    public static void TickFrame(SystemContext* frame)
    {
        int* counters = (int*)frame->User;
        counters[0] += 1;
        counters[1] = (int)frame->FrameIndex;

        Commands commands = Commands.For(frame);
        if (frame->FrameIndex == 0 && counters[6] == 1)
        {
            if (commands.Spawn(out EntityHandle entity))
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
                bool added =
                    commands.Add(entity, Resonate.Hierarchy.LocalTransform.SchemaNameUtf8,
                                 transform);
                counters[6] = added ? 0 : 1;
                counters[7] = (int)entity.Index;
                counters[10] = (int)entity.Generation;
            }
        }
        else if (frame->FrameIndex == 1 && (counters[8] == 1 || counters[8] == 2))
        {
            EntityHandle target = new((uint)counters[7], (uint)counters[10]);
            counters[9] = counters[8] == 1
                              ? (commands.Remove(target,
                                                 Resonate.Hierarchy.LocalTransform.SchemaNameUtf8)
                                     ? 1
                                     : 2)
                              : (commands.Despawn(target) ? 1 : 2);
            counters[8] = 0;
        }

        if (!SelfView<Resonate.Hierarchy.LocalTransform>.TryFrom(
                frame, Resonate.Hierarchy.LocalTransform.SchemaNameUtf8,
                out SelfView<Resonate.Hierarchy.LocalTransform> self))
        {
            counters[2] = -1;
            return;
        }
        using (self)
        {
            Span<Resonate.Hierarchy.LocalTransform> transforms = self.Items;
            float sum = 0.0F;
            for (int index = 0; index < transforms.Length; ++index)
            {
                sum += transforms[index].Position.X;
                transforms[index].Position.Y = 1.0F;
            }
            counters[2] += (int)(sum * 10.0F);
            counters[3] += transforms.Length;
            counters[4] = (int)self.Entities[0].Index;

            /* A view of the wrong type is rejected, so a mismatched T cannot
               alias the chunk. */
            bool rejected = !SelfView<Resonate.Hierarchy.WorldTransform>.TryFrom(
                frame, Resonate.Hierarchy.WorldTransform.SchemaNameUtf8, out _);
            counters[11] = rejected ? 1 : 0;

            if (frame->FrameIndex == 1 && counters[8] == 3)
            {
                counters[9] = commands.Read(self.Entities[0],
                                            Resonate.Hierarchy.LocalTransform.SchemaNameUtf8,
                                            out ReadView<Resonate.Hierarchy.LocalTransform> view)
                                  ? (int)(view.Value.Position.X * 10.0F)
                                  : -1;
                counters[8] = 0;
            }

            /* The view marks the chunk when it is disposed. */
            counters[5] = 1;
        }
    }

    /* The read path's second stage: proves the registration keeps entries
       apart per stage, so the schedule decides when they run. */
    [UpdateIn(ResonateStage.LateUpdate)]
    public static void LateFrame(SystemContext* frame)
    {
        int* counters = (int*)frame->User;
        counters[0] += 10;
        counters[1] = 3;
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
