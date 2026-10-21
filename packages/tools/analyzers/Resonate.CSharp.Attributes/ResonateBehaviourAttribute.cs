using System;

namespace Resonate.CSharp;

/* Marks the assembly whose types the behaviour analyzers police: game state
   lives in native components, so the managed laws only apply there. */
[AttributeUsage(AttributeTargets.Assembly)]
public sealed class ResonateBehaviourAttribute : Attribute
{
}
