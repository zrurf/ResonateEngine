using System;
using System.Collections.Generic;
using System.IO;

namespace Resonate.Tools.Schemac.Cli;

/* The command line over the library: parse the invocation, compile, render
   what came back. Exit codes: 0 success, 1 the schemas are wrong, 2 the
   invocation is. */
internal static class Program
{
    private static int Main(string[] args)
    {
        return Run(args, Console.Out, Console.Error);
    }

    internal static int Run(string[] args, TextWriter output, TextWriter error)
    {
        List<string> roots = [];
        string? outDir = null;
        string? shapeContract = null;
        LockMode lockMode = LockMode.None;

        for (int index = 0; index < args.Length; ++index)
        {
            switch (args[index])
            {
                case "--version":
                    output.WriteLine($"schemac {Schemac.Version}");
                    return 0;
                case "--root":
                    if (++index == args.Length)
                    {
                        return Usage(error, "--root takes a directory");
                    }
                    roots.Add(args[index]);
                    break;
                case "--out":
                    if (++index == args.Length)
                    {
                        return Usage(error, "--out takes a directory");
                    }
                    outDir = args[index];
                    break;
                case "--xsd":
                    if (++index == args.Length)
                    {
                        return Usage(error, "--xsd takes a file");
                    }
                    shapeContract = args[index];
                    break;
                case "--check-lock":
                    if (lockMode == LockMode.Update)
                    {
                        return Usage(error, "--check-lock and --update-lock are exclusive");
                    }
                    lockMode = LockMode.Check;
                    break;
                case "--update-lock":
                    if (lockMode == LockMode.Check)
                    {
                        return Usage(error, "--update-lock and --check-lock are exclusive");
                    }
                    lockMode = LockMode.Update;
                    break;
                default:
                    return Usage(error, $"unknown argument '{args[index]}'");
            }
        }

        if (roots.Count == 0)
        {
            return Usage(error, "--root is required");
        }
        if (outDir is null)
        {
            return Usage(error, "--out is required");
        }

        CompileResult result = Schemac.Compile(roots, outDir, shapeContract, lockMode);
        foreach (Diagnostic diagnostic in result.Diagnostics)
        {
            error.WriteLine(diagnostic.Format());
        }
        if (!result.Success)
        {
            return 1;
        }

        output.WriteLine($"schemac: {result.ModuleCount} module(s), {result.TypeCount} type(s) from "
                         + $"{result.SchemaFileCount} schema file(s), {result.FilesWritten} written, "
                         + $"{result.LocksWritten} lock(s) updated -> {PathText.Relative(outDir)}");
        return 0;
    }

    private static int Usage(TextWriter error, string problem)
    {
        error.WriteLine($"schemac: error: {problem}");
        error.WriteLine("usage: schemac --root <dir> [--root <dir>]... --out <dir> [--xsd <file>] "
                        + "[--check-lock | --update-lock]");
        return 2;
    }
}
