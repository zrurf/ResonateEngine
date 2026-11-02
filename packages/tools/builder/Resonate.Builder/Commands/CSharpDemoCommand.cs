using System.Collections.Generic;
using Resonate.Builder.Tooling;

namespace Resonate.Builder.Commands;

/* Publishes the sample managed game the launcher can run. The demo is content,
   not engine code: it lands in build/content/csharp/demo, and a run points
   [resonate.csharp] at the assembly. */
internal static class CSharpDemoCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate csharp-demo: unknown argument '{args[0]}'");
            return 2;
        }
        if (SchemaCommand.Run(context, []) != 0)
        {
            context.Error.WriteLine("csharp demo: schema generation failed");
            return 1;
        }
        if (CsBindCommand.Run(context, []) != 0)
        {
            context.Error.WriteLine("csharp demo: binding id generation failed");
            return 1;
        }
        try
        {
            DotnetPublisher.Publish(context, "demo",
                context.Path("content", "csharp", "demo", "Resonate.Demo.Game.csproj"),
                context.Path("content", "csharp", "demo"),
                context.BuildPath("content", "csharp", "demo"),
                context.BuildPath("content", "csharp", "demo", "Resonate.Demo.Game.dll"),
                "Debug", readyToRun: false, usesGenerated: true);
        }
        catch (BuilderException error)
        {
            context.Error.WriteLine($"csharp demo: {error.Message}");
            return 1;
        }
        return 0;
    }
}
