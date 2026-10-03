using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using Resonate.Tools.Schemac.Emit;

namespace Resonate.Tools.Schemac.Schema;

/* The version lock: the shape a module's versioned types were last accepted at,
   recorded beside its declarations and committed with them.

   Why it exists: a saved document or a network payload carries a schema version
   and nothing else, so its bytes are read back by whatever the schema says that
   version means. A version that goes backwards, a layout that changes without a
   version bump, or a versioned type that disappears are therefore silent data
   corruption — nothing at run time can notice them. The lock is the record that
   turns them into build failures, and `xmake schema-lock` is the deliberate act
   that accepts a change. */
internal static class VersionLock
{
    internal const int Format = 1;

    private const string FileName = "schemas.lock.json";

    /* A module's lock sits beside its declarations. */
    internal static string PathOf(ModuleUnit module)
    {
        return Path.Combine(CommonDirectory(module.Files), FileName);
    }

    /* Records what the modules' versioned types look like now, answering how
       many locks were (re)written. */
    internal static int Update(IReadOnlyList<ModuleUnit> modules, Diagnostics diagnostics)
    {
        Dictionary<string, string> byPath = new(StringComparer.Ordinal);
        int written = 0;
        foreach (ModuleUnit module in modules)
        {
            string path = PathOf(module);
            if (byPath.TryGetValue(path, out string? other))
            {
                diagnostics.Error($"modules '{other}' and '{module.Id}' both keep their declarations under "
                                  + $"'{PathText.Relative(Path.GetDirectoryName(path)!)}'; a module owns its "
                                  + "directory");
                continue;
            }
            byPath.Add(path, module.Id);

            if (Output.WriteIfChanged(path, Render(module)))
            {
                ++written;
            }
        }
        return written;
    }

    /* Compares the module's versioned types with the lock. Everything the lock
       does not already record — a new type, a different version, a different
       shape, a type that is gone — is an error until `xmake schema-lock`
       records it, so the lock always describes exactly the accepted state. */
    internal static void Check(ModuleUnit module, Diagnostics diagnostics)
    {
        string path = PathOf(module);
        Dictionary<string, LockedType>? locked = Read(path, module, diagnostics);
        if (locked is null)
        {
            return;
        }

        HashSet<string> current = new(StringComparer.Ordinal);
        foreach (TypeDecl type in module.Types)
        {
            if (type.Kind is not (TypeKind.Component or TypeKind.Node))
            {
                continue;
            }
            current.Add(type.Name);

            if (!locked.TryGetValue(type.Name, out LockedType? record))
            {
                diagnostics.Error(type.File, type.Line, type.Column,
                                  $"'{type.Name}' is declared but not in {FileName}; run "
                                      + "'xmake schema-lock' to record it");
                continue;
            }

            if (record.Version != type.Version)
            {
                diagnostics.Error(type.File, type.Line, type.Column, VersionMessage(type, record));
                continue;
            }

            (string? Was, string? Now) difference = FirstDifference(record.Shape, AcceptedShape.Lines(type));
            if (difference.Was is not null || difference.Now is not null)
            {
                diagnostics.Error(type.File, type.Line, type.Column,
                                  $"'{type.Name}': version {type.Version} is recorded with a different shape "
                                      + $"(was '{difference.Was ?? "<nothing>"}', now "
                                      + $"'{difference.Now ?? "<nothing>"}'); a version's layout is frozen - "
                                      + "bump the version and declare the migration, or run 'xmake schema-lock' "
                                      + "if that version never shipped");
            }
        }

        foreach (KeyValuePair<string, LockedType> entry in locked)
        {
            if (!current.Contains(entry.Key))
            {
                diagnostics.Error($"'{entry.Key}' is recorded in {PathText.Relative(path)} but no longer "
                                  + "declared; removing a versioned type is a decision - run "
                                  + "'xmake schema-lock' to record it");
            }
        }
    }

    private static string VersionMessage(TypeDecl type, LockedType record)
    {
        if (record.Version > type.Version)
        {
            return $"'{type.Name}': {FileName} records version {record.Version}, the schema declares "
                   + $"{type.Version}; a version never goes backwards - restore it, or run "
                   + "'xmake schema-lock' if the recorded version was never shipped";
        }

        return $"'{type.Name}': {FileName} records version {record.Version}, the schema declares "
               + $"{type.Version}; run 'xmake schema-lock' to record the new version and commit the lock "
               + "with the change";
    }

    private static (string? Was, string? Now) FirstDifference(List<string> was, List<string> now)
    {
        int count = Math.Max(was.Count, now.Count);
        for (int index = 0; index < count; ++index)
        {
            string? left = index < was.Count ? was[index] : null;
            string? right = index < now.Count ? now[index] : null;
            if (!string.Equals(left, right, StringComparison.Ordinal))
            {
                return (left, right);
            }
        }
        return (null, null);
    }

    private static string Render(ModuleUnit module)
    {
        List<TypeDecl> versioned = [];
        foreach (TypeDecl type in module.Types)
        {
            if (type.Kind is TypeKind.Component or TypeKind.Node)
            {
                versioned.Add(type);
            }
        }
        versioned.Sort(static (left, right) => string.CompareOrdinal(left.Name, right.Name));

        using MemoryStream stream = new();
        using (Utf8JsonWriter writer = new(stream, new JsonWriterOptions
        {
            Indented = true,
            IndentCharacter = ' ',
            IndentSize = 2,
            NewLine = "\n",
        }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("format", Format);
            writer.WriteString("tool", Schemac.Version);
            writer.WriteString("module", module.Id);
            writer.WriteStartArray("types");
            foreach (TypeDecl type in versioned)
            {
                writer.WriteStartObject();
                writer.WriteString("name", type.Name);
                writer.WriteString("kind", AcceptedShape.Word(type.Kind));
                writer.WriteNumber("version", type.Version);
                writer.WriteStartArray("shape");
                foreach (string line in AcceptedShape.Lines(type))
                {
                    writer.WriteStringValue(line);
                }
                writer.WriteEndArray();
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(stream.ToArray()) + "\n";
    }

    private sealed record LockedType(string Name, int Version, List<string> Shape);

    /* Null when there is no usable lock; a missing or unreadable one is reported
       with the command that fixes it. */
    private static Dictionary<string, LockedType>? Read(string path, ModuleUnit module,
                                                        Diagnostics diagnostics)
    {
        if (!File.Exists(path))
        {
            diagnostics.Error($"module '{module.Id}' has no {FileName} under "
                              + $"'{PathText.Relative(Path.GetDirectoryName(path)!)}'; run "
                              + "'xmake schema-lock' to record the accepted shape of its versioned types");
            return null;
        }

        try
        {
            using JsonDocument document = JsonDocument.Parse(File.ReadAllText(path));
            JsonElement root = document.RootElement;
            string lockedModule = root.GetProperty("module").GetString() ?? "";
            if (lockedModule != module.Id)
            {
                diagnostics.Error($"{PathText.Relative(path)} records module '{lockedModule}' but these "
                                  + $"declarations are '{module.Id}'; run 'xmake schema-lock' to rewrite it");
                return null;
            }

            Dictionary<string, LockedType> types = new(StringComparer.Ordinal);
            foreach (JsonElement entry in root.GetProperty("types").EnumerateArray())
            {
                string name = entry.GetProperty("name").GetString() ?? "";
                List<string> shape = [];
                foreach (JsonElement line in entry.GetProperty("shape").EnumerateArray())
                {
                    shape.Add(line.GetString() ?? "");
                }
                types[name] = new LockedType(name, entry.GetProperty("version").GetInt32(), shape);
            }
            return types;
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or IOException
                                      or UnauthorizedAccessException or KeyNotFoundException)
        {
            diagnostics.Error($"{PathText.Relative(path)} cannot be read as a version lock "
                              + $"({error.Message}); run 'xmake schema-lock' to rewrite it");
            return null;
        }
    }

    /* The deepest directory that holds every one of the module's files. */
    private static string CommonDirectory(IReadOnlyList<SchemaFile> files)
    {
        string[] common = Segments(Path.GetDirectoryName(files[0].Path)!);
        foreach (SchemaFile file in files)
        {
            string[] candidate = Segments(Path.GetDirectoryName(file.Path)!);
            int keep = 0;
            while (keep < common.Length && keep < candidate.Length
                   && string.Equals(common[keep], candidate[keep], StringComparison.Ordinal))
            {
                ++keep;
            }
            common = common[..keep];
        }
        return string.Join(Path.DirectorySeparatorChar, common);
    }

    private static string[] Segments(string directory)
    {
        return directory.Split([Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar]);
    }
}
