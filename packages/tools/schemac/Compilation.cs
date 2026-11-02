using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
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
                    CheckOwnership(file, diagnostics);
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
            /* A build reaches several consumers at once and each fires the
               schema hook, so two compiler processes can share one output tree.
               The lock serializes them instead of letting one read a header the
               other is rewriting. */
            Directory.CreateDirectory(outDir);
            using FileStream guard = AcquireLock(Path.Combine(outDir, ".schemac.lock"));
            foreach (ModuleUnit module in modules)
            {
                types += module.Types.Count;
                string header = Path.Combine(outDir, module.Id, "components.gen.h");
                string nodes = Path.Combine(outDir, module.Id, "nodes.gen.h");
                string metadata = Path.Combine(outDir, module.Id + ".schema.json");
                string managed = Path.Combine(outDir, "csharp",
                                              CSharpEmitter.NamespaceOf(module.Id) + ".g.cs");
                if (Output.WriteIfChanged(header, CppEmitter.Render(module)))
                {
                    ++written;
                }
                if (Output.WriteIfChanged(nodes, NodeEmitter.Render(module)))
                {
                    ++written;
                }
                if (Output.WriteIfChanged(metadata, JsonEmitter.Render(module)))
                {
                    ++written;
                }
                if (Output.WriteIfChanged(managed, CSharpEmitter.Render(module)))
                {
                    ++written;
                }
            }
            if (CSharpEmitter.UsesMath(modules))
            {
                string math = Path.Combine(outDir, "csharp", "Resonate.CSharp.Math.g.cs");
                if (Output.WriteIfChanged(math, CSharpEmitter.RenderMath()))
                {
                    ++written;
                }
            }
        }

        return new CompileResult(diagnostics.Items, modules.Count, types, schemas.Count, written,
                                 locks);
    }

    /* A second compiler waits for the one that holds the output tree; the
       timeout is generous because a full compile of a content tree is fast and
       a stuck holder is a bug, not a case to sit on. */
    private static FileStream AcquireLock(string path)
    {
        for (int attempt = 0; ; ++attempt)
        {
            try
            {
                return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite,
                                      FileShare.None);
            }
            catch (IOException) when (attempt < 600)
            {
                Thread.Sleep(100);
            }
        }
    }

    /* A schema under <owner>/schemas belongs to <owner>: the module id is the
       ownership claim the runtime and the C# side use, so a file in another
       module's directory claiming a different id would move the types without
       moving the file. A nested submodule (resonate.gameplay.traps) is still
       owned by the enclosing directory. Roots without a schemas/ level are
       ad-hoc test trees and are left alone. */
    private static void CheckOwnership(SchemaFile file, Diagnostics diagnostics)
    {
        string[] segments = file.Path.Replace('\\', '/').Split('/');
        int schemas = Array.LastIndexOf(segments, "schemas");
        if (schemas <= 0)
        {
            return;
        }
        string owner = segments[schemas - 1];
        string expected = "resonate." + owner;
        if (file.Module == expected ||
            file.Module.StartsWith(expected + ".", StringComparison.Ordinal))
        {
            return;
        }
        diagnostics.Error(file.Path, file.ModuleLine, file.ModuleColumn,
                          $"'{file.Module}' is declared in {owner}/schemas; that directory's "
                              + $"module owns these types ('{expected}')");
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
