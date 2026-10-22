using System;
using System.Runtime.InteropServices;
using Resonate.CSharp;
using Resonate.CSharp.Bindings;

[assembly: ResonateBehaviour]

namespace Resonate.Tests.Bench;

/* Mirrors the native bench's SwarmTransform: the FFI budget compares the same
   dense pass on both sides, so the layout is part of the measurement. */
[StructLayout(LayoutKind.Sequential)]
public struct SwarmTransform
{
    public float Px;
    public float Py;
    public float Pz;
    public float Rx;
    public float Ry;
    public float Rz;
    public float Rw;
    public float Sx;
    public float Sy;
    public float Sz;
}

/* The managed side of the FFI budget. Counter 0 selects the path: the default
   is the self view's write pass (with its dispose mark); 1 is the swarm lane's
   read-only chunk pass. */
public static unsafe class Swarm
{
    [UpdateIn(ResonateStage.Update)]
    public static void Iterate(SystemContext* frame)
    {
        int* counters = (int*)frame->User;
        if (!ChunkList<SwarmTransform>.TryFrom(frame, "bench.SwarmTransform"u8,
                                               out ChunkList<SwarmTransform> chunks))
        {
            return;
        }
        if (counters[0] == 1)
        {
            float sum = 0.0F;
            for (int chunk = 0; chunk < chunks.Count; ++chunk)
            {
                ChunkView<SwarmTransform> read = chunks.Read(chunk);
                ReadOnlySpan<SwarmTransform> rows = read.Items;
                for (int index = 0; index < rows.Length; ++index)
                {
                    sum += rows[index].Px;
                }
            }
            counters[1] = (int)sum;
            return;
        }

        for (int chunk = 0; chunk < chunks.Count; ++chunk)
        {
            using (SelfView<SwarmTransform> view = chunks.Self(chunk))
            {
                Span<SwarmTransform> rows = view.Items;
                for (int index = 0; index < rows.Length; ++index)
                {
                    rows[index].Py = rows[index].Px;
                }
            }
        }
    }
}
