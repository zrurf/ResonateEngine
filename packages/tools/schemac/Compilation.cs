using System;
using System.Collections.Generic;
using System.IO;
using System.Xml.Schema;
using Resonate.Tools.Schemac.Emit;
using Resonate.Tools.Schemac.Schema;

namespace Resonate.Tools.Schemac;

internal static class Compilation
{
    /* Returns the run's data and leaves rendering to the caller (the CLI
       prints; a host would show it somewhere else). */
    internal static CompileResult Run(IReadOnlyList<string> roots, string outDir,
                                      string? shapeContractPath, LockMode lockMode)
    {
        Diagnostics diagnostics = new();

        XmlSchemaSet? contract = null;
        if (shapeContractPath is not null)
        {
            contract = ShapeContract.Load(shapeContractPath, diagnostics);
        }

        List<string> schemas = Discover(roots, diagnostics);

        if (!diagnostics.HasErrors && schemas.Count == 0)
        {
            diagnostics.Error("no .rschema files found under the given roots");
        }

        List<SchemaFile> files = [];
        if (!diagnostics.HasErrors)
        {
            foreach (string path in schemas)
            {
                SchemaFile? file = SchemaParser.Parse(path, diagnostics);
                if (file is not null)
                {
                    files.Add(file);
                }
            }
        }

        /* Cross-file checks run only on a clean parse: a half-built declaration
           would make resolution report second-order noise. */
        List<ModuleUnit> modules = [];
        if (!diagnostics.HasErrors)
        {
            modules = SchemaCompiler.Compile(files, diagnostics);
        }

        /* The shape contract is checked after the compiler's own pass, so a
           plain mistake reports the compiler's message and the only thing this
           check can add is drift: a declaration the compiler accepts that the
           contract does not know about. */
        if (contract is not null && !diagnostics.HasErrors)
        {
            foreach (string path in schemas)
            {
                ShapeContract.Validate(path, contract, diagnostics);
            }
        }

        /* The version locks are the other side of the same idea: what a reader
           of saved data sees has to stay what the schema says, so anything the
           lock does not already record fails until it is recorded on purpose. */
        int locks = 0;
        if (lockMode == LockMode.Update && !diagnostics.HasErrors)
        {
            locks = VersionLock.Update(modules, diagnostics);
        }
        if (lockMode == LockMode.Check && !diagnostics.HasErrors)
        {
            foreach (ModuleUnit module in modules)
            {
                VersionLock.Check(module, diagnostics);
            }
        }

        /* Nothing is written on a failed run: the previous output stays, so a
           broken edit cannot leave half-updated generated types behind. */
        int types = 0;
        int written = 0;
        if (!diagnostics.HasErrors)
        {
            foreach (ModuleUnit module in modules)
            {
                types += module.Types.Count;
                string header = Path.Combine(outDir, module.Id, "components.gen.h");
                string metadata = Path.Combine(outDir, module.Id + ".schema.json");
                if (Output.WriteIfChanged(header, CppEmitter.Render(module)))
                {
                    ++written;
                }
                if (Output.WriteIfChanged(metadata, JsonEmitter.Render(module)))
                {
                    ++written;
                }
            }
        }

        return new CompileResult(diagnostics.Items, modules.Count, types, schemas.Count, written,
                                 locks);
    }

    /* A root that is not a directory is a build-configuration mistake, not an
       empty module, so it fails loudly instead of compiling nothing. */
    private static List<string> Discover(IReadOnlyList<string> roots, Diagnostics diagnostics)
    {
        List<string> files = [];
        foreach (string root in roots)
        {
            if (!Directory.Exists(root))
            {
                diagnostics.Error($"root '{root}' is not a directory");
                continue;
            }

            files.AddRange(Directory.EnumerateFiles(root, "*.rschema", SearchOption.AllDirectories));
        }

        files.Sort(StringComparer.Ordinal);
        return files;
    }
}
