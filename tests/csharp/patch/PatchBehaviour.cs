using Resonate.CSharp;

[assembly: ResonateBehaviour]

namespace Resonate.Tests.Patch;

/* A patch assembly: the generator emits its registration, which the patch host
   invokes reflectively and which replaces the engine's UPDATE slot. */
public static unsafe class PatchBehaviour
{
    /* The patched entry: it reports itself through the native counters. */
    [UpdateIn(ResonateStage.Update)]
    public static void Tick(Resonate.CSharp.Bindings.SystemContext* frame)
    {
        int* counters = (int*)frame->User;
        counters[0] += 1;
        counters[1] = 2;
    }
}
