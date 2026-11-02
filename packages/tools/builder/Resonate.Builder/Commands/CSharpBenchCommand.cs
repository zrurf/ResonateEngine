using System.Collections.Generic;
using Resonate.Builder.Tooling;

namespace Resonate.Builder.Commands;

/* Publishes the managed FFI-bench assembly in Release and ReadyToRun: the
   budget compares optimized native code, so a debug publish would measure the
   wrong thing. */
internal static class CSharpBenchCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate csharp-bench: unknown argument '{args[0]}'");
            return 2;
        }
        if (SchemaCommand.Run(context, []) != 0)
        {
            context.Error.WriteLine("csharp bench: schema generation failed");
            return 1;
        }
        if (CsBindCommand.Run(context, []) != 0)
        {
            context.Error.WriteLine("csharp bench: binding id generation failed");
            return 1;
        }
        try
        {
            DotnetPublisher.Publish(context, "bench",
                context.Path("tests", "csharp", "bench", "Resonate.Tests.Bench.csproj"),
                context.Path("tests", "csharp", "bench"),
                context.BuildPath("tools", "csharp-bench"),
                context.BuildPath("tools", "csharp-bench", "Resonate.Tests.Bench.dll"),
                "Release", readyToRun: true, usesGenerated: true);
        }
        catch (BuilderException error)
        {
            context.Error.WriteLine($"csharp bench: {error.Message}");
            return 1;
        }
        return 0;
    }
}
