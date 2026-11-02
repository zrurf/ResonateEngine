using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace Resonate.Builder.Tooling;

/* Compiles the modules' Slang shaders to SPIR-V plus reflection JSON. The
   entry points come from the [shader("...")] attributes in each source, and
   slangc validates the SPIR-V it emits, so a green exit is the compile check.
   The incremental rule is the source's mtime: both outputs must be newer. */
internal static class ShaderCompiler
{
    internal static int Run(BuildContext context)
    {
        string? sdk = Environment.GetEnvironmentVariable("RESONATE_VULKAN_SDK");
        if (string.IsNullOrEmpty(sdk))
        {
            sdk = Environment.GetEnvironmentVariable("VULKAN_SDK");
        }
        if (string.IsNullOrEmpty(sdk))
        {
            throw new BuilderException("slang: no Vulkan SDK in the environment (VULKAN_SDK), "
                                       + "so no slangc");
        }
        string slangc = Path.Combine(sdk, "Bin", OperatingSystem.IsWindows() ? "slangc.exe"
                                                                             : "slangc");
        if (!File.Exists(slangc))
        {
            throw new BuilderException($"slang: slangc not found at {slangc}");
        }

        string output = context.BuildPath("shaders");
        Directory.CreateDirectory(output);
        List<string> sources = [];
        string modules = context.Path("packages", "modules");
        if (Directory.Exists(modules))
        {
            foreach (string module in Directory.GetDirectories(modules))
            {
                sources.AddRange(Files(Path.Combine(module, "shaders"), "*.slang"));
            }
        }
        sources.AddRange(Files(context.Path("packages", "renderer", "shaders"), "*.slang"));
        sources.Sort(StringComparer.Ordinal);

        int shaders = 0;
        int compiled = 0;
        int entries = 0;
        foreach (string source in sources)
        {
            ++shaders;
            string name = Path.GetFileName(source);
            DateTime sourceTime = File.GetLastWriteTimeUtc(source);
            List<(string Stage, string Name)> points = EntryPoints(File.ReadAllText(source));
            if (points.Count == 0)
            {
                throw new BuilderException($"slang: {source} declares no [shader(...)] entry points");
            }
            foreach ((string stage, string entry) in points)
            {
                ++entries;
                string spv = Path.Combine(output, name + "." + entry + ".spv");
                string reflection = Path.Combine(output, name + "." + entry + ".refl.json");
                if (UpToDate(spv, sourceTime) && UpToDate(reflection, sourceTime))
                {
                    continue;
                }
                /* Row-major storage with column vectors matches the engine's
                   matrices. SPIR-V 1.3 is the newest Vulkan 1.1 accepts, the
                   backend's floor. */
                ProcessRunner.MustRun(slangc, [source, "-entry", entry, "-target", "spirv",
                                               "-profile", "spirv_1_3", "-matrix-layout-row-major",
                                               "-o", spv, "-reflection-json", reflection]);
                ++compiled;
            }
        }
        context.Output.WriteLine($"slangc: {shaders} shader(s), {entries} entry point(s), "
                                 + $"{compiled} written -> {Relative(context, output)}");
        return 0;
    }

    /* '[shader("vertex")]' names the stage; the first function definition line
       after it is the entry point. Line-based, because a single pattern across
       the whole source cannot stop before the entry-point name greedily. */
    private static List<(string Stage, string Name)> EntryPoints(string source)
    {
        List<(string, string)> entries = [];
        string? stage = null;
        foreach (string rawLine in source.Split('\n'))
        {
            string line = rawLine.TrimEnd('\r');
            if (Regex.IsMatch(line, @"^\s*\[[A-Za-z]"))
            {
                if (stage is null)
                {
                    Match match = Regex.Match(line,
                        @"^\s*\[shader\s*\(\s*""(\w+)""\s*\)\s*\]");
                    if (match.Success)
                    {
                        stage = match.Groups[1].Value;
                    }
                }
            }
            else if (stage is not null && !Regex.IsMatch(line, @"^\s*//"))
            {
                Match match = Regex.Match(line, @"^\s*(.*?)(\w+)\s*\(");
                if (match.Success)
                {
                    entries.Add((stage, match.Groups[2].Value));
                    stage = null;
                }
            }
        }
        return entries;
    }

    private static bool UpToDate(string path, DateTime sourceTime)
    {
        return File.Exists(path) && File.GetLastWriteTimeUtc(path) > sourceTime;
    }

    private static List<string> Files(string directory, string pattern)
    {
        if (!Directory.Exists(directory))
        {
            return [];
        }
        List<string> files = Directory.GetFiles(directory, pattern).ToList();
        files.Sort(StringComparer.Ordinal);
        return files;
    }

    private static string Relative(BuildContext context, string path)
    {
        return Path.GetRelativePath(context.ProjectDirectory, path).Replace('\\', '/');
    }
}
