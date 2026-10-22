using System;

namespace Resonate.CSharp;

/* Law 4: an entry point runs only where it says. The registration generator
   registers methods carrying this attribute and warns about entry-shaped
   methods that lack it. */
[AttributeUsage(AttributeTargets.Method)]
public sealed class UpdateInAttribute : Attribute
{
    public UpdateInAttribute(ResonateStage stage)
    {
        Stage = stage;
    }

    public ResonateStage Stage { get; }
}
