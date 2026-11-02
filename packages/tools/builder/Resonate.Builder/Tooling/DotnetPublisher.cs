using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace Resonate.Builder.Tooling;

/* Publishes a managed project with the stale-input rules the fixture scripts
   used: the assembly's mtime cannot record a publish whose copy MSBuild
   skipped, so a stamp written after every successful publish is the reference.
   The generated ids and the analyzer sources are inputs of every behaviour
   assembly, so their mtimes count too. */
internal static class DotnetPublisher
{
    internal static void Publish(BuildContext context, string name, string project,
                                 string sourceDirectory, string outDirectory, string assembly,
                                 string configuration, bool readyToRun, bool usesGenerated)
    {
        if (!IsStale(context, sourceDirectory, assembly, usesGenerated))
        {
            return;
        }
        List<string> arguments = ["publish", project, "-c", configuration, "-o", outDirectory,
                                  "--nologo", "-v", "quiet"];
        if (readyToRun)
        {
            arguments.AddRange(["-r", RuntimeIdentifier(), "--self-contained", "false",
                                "-p:PublishReadyToRun=true"]);
        }
        int status = ProcessRunner.Run("dotnet", arguments);
        if (status != 0)
        {
            /* Two builds can publish the same output at once; a retry finds the
               winner's assembly in place. */
            System.Threading.Thread.Sleep(1000);
            status = ProcessRunner.Run("dotnet", arguments);
        }
        if (status == 0)
        {
            File.WriteAllText(assembly + ".stamp", DateTime.UtcNow.Ticks.ToString());
        }
        if (IsStale(context, sourceDirectory, assembly, usesGenerated))
        {
            throw new BuilderException(
                $"csharp {name}: publishing failed (dotnet publish exited {status}); the .NET SDK "
                + "is required");
        }
    }

    internal static string RuntimeIdentifier()
    {
        string architecture = RuntimeInformation.OSArchitecture switch
        {
            Architecture.Arm64 => "arm64",
            Architecture.X86 => "x86",
            _ => "x64",
        };
        if (OperatingSystem.IsWindows())
        {
            return "win-" + architecture;
        }
        if (OperatingSystem.IsMacOS())
        {
            return "osx-" + architecture;
        }
        return "linux-" + architecture;
    }

    private static bool IsStale(BuildContext context, string sourceDirectory, string assembly,
                               bool usesGenerated)
    {
        if (!File.Exists(assembly))
        {
            return true;
        }
        string stamp = assembly + ".stamp";
        DateTime builtAt = File.Exists(stamp)
            ? File.GetLastWriteTimeUtc(stamp)
            : File.GetLastWriteTimeUtc(assembly);
        if (AnyNewer(sourceDirectory, "**", builtAt))
        {
            return true;
        }
        if (usesGenerated && AnyNewer(context.BuildPath("gen", "csharp"), "*.g.cs", builtAt))
        {
            return true;
        }
        return AnyNewer(context.Path("packages", "tools", "analyzers"), "**", builtAt);
    }

    private static bool AnyNewer(string directory, string pattern, DateTime builtAt)
    {
        if (!Directory.Exists(directory))
        {
            return false;
        }
        string search = pattern == "**" ? "*" : pattern;
        foreach (string file in Directory.GetFiles(directory, search, SearchOption.AllDirectories))
        {
            if (File.GetLastWriteTimeUtc(file) >= builtAt)
            {
                return true;
            }
        }
        return false;
    }
}
