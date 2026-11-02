using System;
using System.Collections.Generic;
using Resonate.Tools.CsBind;

namespace Resonate.Builder.Commands;

/* Generates the stable system ids and the boundary structs both sides compile
   against. The generator is a library call: the contract is content, and a
   build step should not start a second .NET process for it. */
internal static class CsBindCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate csbind: unknown argument '{args[0]}'");
            return 2;
        }
        string input = context.Path("content", "csharp", "bindings.json");
        string outDirectory = context.BuildPath("gen", "csharp");
        int status = BindingGenerator.Run(input, outDirectory);
        if (status == 0)
        {
            context.Output.WriteLine($"csbind: {input} -> {outDirectory}");
        }
        return status;
    }
}
