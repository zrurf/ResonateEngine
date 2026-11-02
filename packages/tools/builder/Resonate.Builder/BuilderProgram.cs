using System;
using System.Collections.Generic;
using System.IO;

namespace Resonate.Builder;

/* The project the commands operate on: the repository root, the build root and
   the two writers diagnostics go to. xmake passes neither path as an option in
   normal use -- the builder walks up from the working directory, which is what
   a developer running `resonate cook` expects -- and both are explicit for a
   caller that drives the builder from somewhere else. */
public sealed class BuildContext
{
    public BuildContext(string projectDirectory, string buildDirectory, TextWriter output,
                        TextWriter error)
    {
        ProjectDirectory = projectDirectory;
        BuildDirectory = buildDirectory;
        Output = output;
        Error = error;
    }

    public string ProjectDirectory { get; }

    public string BuildDirectory { get; }

    public TextWriter Output { get; }

    public TextWriter Error { get; }

    public string Path(params string[] parts)
    {
        string path = ProjectDirectory;
        foreach (string part in parts)
        {
            path = System.IO.Path.Combine(path, part);
        }
        return path;
    }

    public string BuildPath(params string[] parts)
    {
        string path = BuildDirectory;
        foreach (string part in parts)
        {
            path = System.IO.Path.Combine(path, part);
        }
        return path;
    }
}

public static class BuilderProgram
{
    public static int Run(IReadOnlyList<string> args, TextWriter output, TextWriter error)
    {
        string? project = null;
        string? build = null;
        int at = 0;
        while (at < args.Count)
        {
            if (args[at] == "--project" && at + 1 < args.Count)
            {
                project = args[at + 1];
                at += 2;
            }
            else if (args[at] == "--build" && at + 1 < args.Count)
            {
                build = args[at + 1];
                at += 2;
            }
            else
            {
                break;
            }
        }

        string root = project is null ? FindProjectDirectory() : System.IO.Path.GetFullPath(project);
        string buildRoot = build is null
            ? System.IO.Path.Combine(root, "build")
            : System.IO.Path.GetFullPath(build);
        BuildContext context = new(root, buildRoot, output, error);

        if (at >= args.Count)
        {
            return Usage(error);
        }
        string command = args[at++];
        List<string> rest = [];
        for (int index = at; index < args.Count; ++index)
        {
            rest.Add(args[index]);
        }
        switch (command)
        {
            case "schema":
                return Commands.SchemaCommand.Run(context, rest);
            case "csbind":
                return Commands.CsBindCommand.Run(context, rest);
            case "module-check":
                return Commands.ModuleCheckCommand.Run(context, rest);
            case "package":
                return Commands.PackageCommand.Run(context, rest);
            case "selfcheck":
                return Commands.SelfCheckCommand.Run(context, rest);
            case "assets":
                return Commands.AssetsCommand.Run(context, rest);
            case "shaders":
                return Commands.ShadersCommand.Run(context, rest);
            case "format":
                return Commands.FormatCommand.Run(context, rest);
            case "analyzers":
                return Commands.AnalyzersCommand.Run(context, rest);
            case "csharp-test":
                return Commands.CSharpTestCommand.Run(context, rest);
            case "csharp-demo":
                return Commands.CSharpDemoCommand.Run(context, rest);
            case "csharp-bench":
                return Commands.CSharpBenchCommand.Run(context, rest);
            case "csharp-reload":
                return Commands.CSharpReloadCommand.Run(context, rest);
            case "rust":
                return Commands.RustCommand.Run(context, rest);
            case "tgfx":
                return Commands.TgfCommand.Run(context, rest);
            default:
                error.WriteLine($"resonate: unknown command '{command}'");
                return Usage(error);
        }
    }

    private static int Usage(TextWriter error)
    {
        error.WriteLine("usage: resonate <command> [options]");
        error.WriteLine("  schema   compile content .rschema declarations into generated types");
        error.WriteLine("  csbind   generate the managed/engine binding ids from the contract");
        error.WriteLine("  module-check  validate module manifests against descriptors and headers");
        error.WriteLine("  package android  assemble and sign the APK from an Android build");
        error.WriteLine("  package desktop  assemble a shipped desktop tree from a build");
        error.WriteLine("  selfcheck  run the builder's own checks");
        error.WriteLine("  assets   index the content sidecars into the products manifest");
        error.WriteLine("  shaders  compile Slang shaders to SPIR-V and reflection");
        error.WriteLine("  format   run clang-format over the source tree");
        error.WriteLine("  analyzers  run the behaviour analyzers' self-checks");
        error.WriteLine("  csharp-test|demo|bench|reload  publish the managed fixtures");
        error.WriteLine("  rust     build the text island's Rust library");
        error.WriteLine("  tgfx     fetch and build tgfx");
        error.WriteLine("options: --project <dir> --build <dir>");
        return 2;
    }

    /* The repository root: the nearest ancestor that has both the project file
       and the content tree. A developer runs the builder from anywhere inside
       the project; a toolchain that starts it elsewhere passes --project. */
    private static string FindProjectDirectory()
    {
        string? directory = Environment.CurrentDirectory;
        while (directory is not null)
        {
            if (File.Exists(System.IO.Path.Combine(directory, "xmake.lua")) &&
                Directory.Exists(System.IO.Path.Combine(directory, "content")))
            {
                return directory;
            }
            directory = Directory.GetParent(directory)?.FullName;
        }
        return Environment.CurrentDirectory;
    }
}
