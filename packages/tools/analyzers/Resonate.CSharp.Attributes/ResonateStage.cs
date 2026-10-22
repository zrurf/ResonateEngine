namespace Resonate.CSharp;

/* Mirrors ResonateStage in module/stage.h. The registration generator maps a
   stage to the stable system id in the binding contract, so the enum values
   are ABI and must not be reordered. */
public enum ResonateStage : byte
{
    EarlyUpdate = 1,
    Update = 2,
    Physics = 3,
    LateUpdate = 4,
    Render = 5,
    Present = 6,
}
