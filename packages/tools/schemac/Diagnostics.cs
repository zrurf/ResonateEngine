using System;
using System.Collections.Generic;
using System.IO;

namespace Resonate.Tools.Schemac;

public enum Severity
{
    Warning,
    Error,
}

/* One problem, positioned when it came from a file. */
public sealed record Diagnostic(Severity Severity, string? File, int Line, int Column,
                                string Message)
{
    public string Format()
    {
        string level = Severity == Severity.Error ? "error" : "warning";
        if (File is null)
        {
            return $"schemac: {level}: {Message}";
        }
        return $"{PathText.Relative(File)}:{Line}:{Column}: {level}: {Message}";
    }
}

/* The problems of one run. Schemas are edited by hand, so a run collects every
   problem it found rather than stopping at the first, and the build gate is
   "any error at all". */
internal sealed class Diagnostics
{
    private readonly List<Diagnostic> _items = [];

    internal bool HasErrors { get; private set; }

    internal IReadOnlyList<Diagnostic> Items => _items;

    internal void Error(string file, int line, int column, string message)
    {
        Add(new Diagnostic(Severity.Error, file, line, column, message));
    }

    /* A problem about the invocation or the tree rather than one file. */
    internal void Error(string message)
    {
        Add(new Diagnostic(Severity.Error, null, 0, 0, message));
    }

    internal void Warning(string file, int line, int column, string message)
    {
        Add(new Diagnostic(Severity.Warning, file, line, column, message));
    }

    private void Add(Diagnostic item)
    {
        _items.Add(item);
        if (item.Severity == Severity.Error)
        {
            HasErrors = true;
        }
    }
}

/* The tool's rendering of a path: relative to the working directory and with
   forward slashes, so diagnostics, generated files and metadata read the same
   on every platform. */
public static class PathText
{
    public static string Relative(string path)
    {
        string relative = Path.GetRelativePath(Environment.CurrentDirectory, path);
        if (relative.StartsWith("..", StringComparison.Ordinal))
        {
            relative = path;
        }
        return relative.Replace('\\', '/');
    }
}
