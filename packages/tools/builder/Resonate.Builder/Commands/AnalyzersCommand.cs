using System.Collections.Generic;

namespace Resonate.Builder.Commands;

/* Builds and runs the Roslyn analyzers' self-checks. The behaviour laws are
   build-time contracts, so the analyzers carry their own program with a
   non-zero exit; the test target and `xmake analyzers` both come through
   here. */
internal static class AnalyzersCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate analyzers: unknown argument '{args[0]}'");
            return 2;
        }
        string project = context.Path("tests", "csharp", "analyzers",
                                      "Resonate.Analyzers.Tests.csproj");
        int status = ProcessRunner.Run("dotnet", ["run", "--project", project, "-c", "Debug",
                                                  "--nologo", "-v", "quiet"]);
        if (status != 0)
        {
            context.Error.WriteLine($"analyzers: the self-check failed (exit {status}); the .NET SDK is required");
            return 1;
        }
        return 0;
    }
}
