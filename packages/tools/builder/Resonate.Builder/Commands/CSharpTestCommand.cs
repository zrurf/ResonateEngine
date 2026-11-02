using System.Collections.Generic;
using Resonate.Builder.Tooling;

namespace Resonate.Builder.Commands;

/* Prepares everything the C# host tests load: the managed fixture (published
   with dynamic loading so hostfxr finds its runtimeconfig), the patch host
   bootstrap and a patch assembly. The analyzers' self-check runs last, because
   the laws are build-time contracts and the suite is where they are enforced. */
internal static class CSharpTestCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate csharp-test: unknown argument '{args[0]}'");
            return 2;
        }
        if (SchemaCommand.Run(context, []) != 0)
        {
            context.Error.WriteLine("csharp test: schema generation failed");
            return 1;
        }
        if (CsBindCommand.Run(context, []) != 0)
        {
            context.Error.WriteLine("csharp test: binding id generation failed");
            return 1;
        }
        try
        {
            PublishFixture(context);
            PublishBootstrap(context);
            PublishPatch(context);
        }
        catch (BuilderException error)
        {
            context.Error.WriteLine($"csharp test: {error.Message}");
            return 1;
        }
        return AnalyzersCommand.Run(context, []);
    }

    private static void PublishFixture(BuildContext context)
    {
        DotnetPublisher.Publish(context, "the fixture",
            context.Path("tests", "csharp", "managed", "Resonate.Tests.Managed.csproj"),
            context.Path("tests", "csharp", "managed"),
            context.BuildPath("tools", "csharp-test"),
            context.BuildPath("tools", "csharp-test", "Resonate.Tests.Managed.dll"),
            "Debug", readyToRun: true, usesGenerated: true);
    }

    private static void PublishBootstrap(BuildContext context)
    {
        DotnetPublisher.Publish(context, "the patch host",
            context.Path("tests", "csharp", "bootstrap", "Resonate.Tests.Bootstrap.csproj"),
            context.Path("tests", "csharp", "bootstrap"),
            context.BuildPath("tools", "csharp-bootstrap"),
            context.BuildPath("tools", "csharp-bootstrap", "Resonate.Tests.Bootstrap.dll"),
            "Debug", readyToRun: false, usesGenerated: false);
    }

    private static void PublishPatch(BuildContext context)
    {
        DotnetPublisher.Publish(context, "the patch",
            context.Path("tests", "csharp", "patch", "Resonate.Tests.Patch.csproj"),
            context.Path("tests", "csharp", "patch"),
            context.BuildPath("tools", "csharp-patch"),
            context.BuildPath("tools", "csharp-patch", "Resonate.Tests.Patch.dll"),
            "Debug", readyToRun: false, usesGenerated: true);
    }
}
