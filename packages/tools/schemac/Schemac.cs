using System.Collections.Generic;

namespace Resonate.Tools.Schemac;

/* What a run does with the version locks (the recorded accepted shape of every
   versioned type, beside each module's declarations). */
public enum LockMode
{
    /* Neither checks nor writes them: the mode tests and one-off runs use. */
    None,

    /* Fails the run for anything the lock does not already record. */
    Check,

    /* Rewrites the locks from what the schemas say now. The deliberate act that
       accepts a version bump or a removed type. */
    Update,
}

/* The compiler's in-process entry. The build's command line runs it, and so
   will the editor when it compiles schemas itself rather than shelling out to
   the binary: diagnostics come back as data, because rendering them is the
   caller's business. */
public static class Schemac
{
    /* Bumped whenever the generated output changes shape. The generated files
       and the metadata carry it, so a consumer can tell which tool wrote a
       file, and an upgraded tool rewrites what it finds. */
    public const string Version = "0.1.0";

    /* Reads the .rschema declarations under `roots`, checks them against each
       other, and writes each module's generated C++ header and metadata into
       `outputDirectory`. Never throws for a problem in the schemas: everything
       comes back in the result's diagnostics.

       `shapeContract` is the XSD a declaration file's own shape is checked
       against as well (schema/formats/schema/rschema.xsd); null skips that
       check. The compiler's checks are the stricter half, so this only ever
       fails a declaration the compiler accepted and the contract does not know
       about - which is what keeps the two definitions from drifting.

       `lockMode` decides what happens with the version locks; see LockMode. */
    public static CompileResult Compile(IReadOnlyList<string> roots, string outputDirectory,
                                        string? shapeContract = null,
                                        LockMode lockMode = LockMode.None)
    {
        return Compilation.Run(roots, outputDirectory, shapeContract, lockMode);
    }
}

/* What one run produced: the diagnostics in report order, and the counts a
   caller needs to say what happened. */
public sealed class CompileResult
{
    internal CompileResult(IReadOnlyList<Diagnostic> diagnostics, int moduleCount, int typeCount,
                           int schemaFileCount, int filesWritten, int locksWritten)
    {
        Diagnostics = diagnostics;
        ModuleCount = moduleCount;
        TypeCount = typeCount;
        SchemaFileCount = schemaFileCount;
        FilesWritten = filesWritten;
        LocksWritten = locksWritten;
    }

    public IReadOnlyList<Diagnostic> Diagnostics { get; }

    /* Valid on a successful run; zero on a failed one. */
    public int ModuleCount { get; }
    public int TypeCount { get; }
    public int SchemaFileCount { get; }
    public int FilesWritten { get; }

    /* Non-zero only in LockMode.Update. */
    public int LocksWritten { get; }

    /* An error fails the run; a warning is reported and lets it pass. */
    public bool Success
    {
        get
        {
            foreach (Diagnostic diagnostic in Diagnostics)
            {
                if (diagnostic.Severity == Severity.Error)
                {
                    return false;
                }
            }
            return true;
        }
    }
}
