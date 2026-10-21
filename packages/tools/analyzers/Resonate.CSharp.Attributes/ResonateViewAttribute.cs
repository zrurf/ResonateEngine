using System;

namespace Resonate.CSharp;

/* Marks a type generated from a schema as a component view: a view borrows the
   chunk it describes, so a behaviour assembly must read it where the engine
   hands it over instead of keeping it. */
[AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class)]
public sealed class ResonateViewAttribute : Attribute
{
}
