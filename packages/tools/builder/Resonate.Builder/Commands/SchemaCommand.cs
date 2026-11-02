using System;
using System.Collections.Generic;
using Resonate.Tools.Schemac;

namespace Resonate.Builder.Commands;

/* Compiles the content modules' .rschema declarations into the generated C++
   and C# headers plus the metadata artifacts, and checks the version locks
   unless the run is the deliberate update. The compiler is a library here:
   the builder is the only published entry point, so no second process has to
   be started for a build step. */
internal static class SchemaCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        LockMode lockMode = LockMode.Check;
        for (int index = 0; index < args.Count; ++index)
        {
            switch (args[index])
            {
                case "--update-lock":
                    lockMode = LockMode.Update;
                    break;
                default:
                    context.Error.WriteLine($"resonate schema: unknown argument '{args[index]}'");
                    return 2;
            }
        }

        string root = context.Path("content");
        string outDirectory = context.BuildPath("gen");
        string shapeContract = context.Path("schema", "formats", "schema", "rschema.xsd");
        CompileResult result = Schemac.Compile([root], outDirectory, shapeContract, lockMode);
        foreach (Diagnostic diagnostic in result.Diagnostics)
        {
            context.Error.WriteLine(diagnostic.Format());
        }
        if (!result.Success)
        {
            return 1;
        }
        context.Output.WriteLine($"schemac: {result.ModuleCount} module(s), {result.TypeCount} "
                                 + $"type(s) from {result.SchemaFileCount} schema file(s), "
                                 + $"{result.FilesWritten} written, {result.LocksWritten} lock(s) "
                                 + $"updated -> {PathText.Relative(outDirectory)}");
        return 0;
    }
}
