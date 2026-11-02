using System;
using System.Collections.Generic;
using System.IO;

namespace Resonate.Builder.Commands;

/* Runs clang-format over the source tree. Kept explicit rather than a build
   step so a reformatted tree is always a deliberate commit. */
internal static class FormatCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate format: unknown argument '{args[0]}'");
            return 2;
        }
        string? formatter = Environment.GetEnvironmentVariable("RESONATE_CLANG_FORMAT");
        if (string.IsNullOrEmpty(formatter))
        {
            foreach (string candidate in new[]
                     {
                         "clang-format", "clang-format-19", "clang-format-18", "clang-format-17",
                     })
            {
                if (ProcessRunner.Run(candidate, ["--version"]) == 0)
                {
                    formatter = candidate;
                    break;
                }
            }
        }
        if (string.IsNullOrEmpty(formatter))
        {
            context.Output.WriteLine(
                "clang-format not found; set RESONATE_CLANG_FORMAT to run this");
            return 0;
        }

        List<string> files = [];
        foreach (string root in new[] { "packages", "tests" })
        {
            string directory = context.Path(root);
            if (!Directory.Exists(directory))
            {
                continue;
            }
            foreach (string pattern in new[] { "*.h", "*.hpp", "*.cpp" })
            {
                files.AddRange(Directory.GetFiles(directory, pattern, SearchOption.AllDirectories));
            }
        }
        files.Sort(StringComparer.Ordinal);
        int count = 0;
        int failed = 0;
        foreach (string file in files)
        {
            if (ProcessRunner.Run(formatter, ["-i", "--style=file", file]) == 0)
            {
                ++count;
            }
            else
            {
                ++failed;
                context.Error.WriteLine($"clang-format failed on {file}");
            }
        }
        context.Output.WriteLine($"formatted {count} file(s)");
        if (failed != 0)
        {
            context.Error.WriteLine($"clang-format failed on {failed} file(s)");
            return 1;
        }
        return 0;
    }
}
