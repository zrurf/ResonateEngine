using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace Resonate.Builder.ModuleCheck;

/* One module's manifest plus where it came from. */
internal sealed class ModuleManifest
{
    public required Dictionary<string, object?> Fields { get; init; }
    public required string Directory { get; init; }
    public required string Source { get; init; }

    public string? Id => Fields.TryGetValue("id", out object? value) ? value as string : null;

    public string Label => Id ?? Directory;

    public string? Text(string field)
    {
        return Fields.TryGetValue(field, out object? value) ? value as string : null;
    }

    public long Number(string field)
    {
        return Fields.TryGetValue(field, out object? value) && value is long number ? number : 0;
    }

    public List<string> Strings(string field)
    {
        List<string> result = [];
        if (Fields.TryGetValue(field, out object? value) && value is List<object?> items)
        {
            foreach (object? item in items)
            {
                if (item is not null)
                {
                    result.Add(item.ToString() ?? string.Empty);
                }
            }
        }
        return result;
    }
}

/* The four places a module's contract is written down -- the manifest, the C++
   descriptor, the capability headers and the build table -- are cross-checked
   here. Any disagreement fails the build; the checks and their messages are the
   ones scripts/module_check.lua performed. */
internal sealed class Checker
{
    private static readonly string[] ModuleFields =
    [
        "id", "name", "version", "abi", "min_host_abi", "summary", "provides", "requires",
        "optional", "depends_on",
    ];

    private readonly BuildContext _context;
    private readonly List<string> _problems = [];

    internal Checker(BuildContext context)
    {
        _context = context;
    }

    internal int Execute()
    {
        Dictionary<string, (string Type, string Header)> declared = ScanDeclaredCapabilities();
        string hostPath = _context.Path("resonate.host.toml");
        Manifest? hostManifest = File.Exists(hostPath) ? Manifest.Read(hostPath) : null;
        if (hostManifest is not null)
        {
            foreach (string problem in hostManifest.Problems)
            {
                Problem(problem);
            }
        }
        Dictionary<string, object?>? host = hostManifest is null
            ? null
            : SingleSection(hostManifest, "resonate.host", "resonate.host.toml");
        if (host is null)
        {
            return 0;
        }

        RequireFields(host, "resonate.host.toml", ["id", "name", "abi", "summary", "provides"]);
        RequireNoExtraFields(host, "resonate.host.toml", ["id", "name", "abi", "summary", "provides"]);

        List<ModuleManifest> modules = [];
        Dictionary<string, ModuleManifest> byId = new(StringComparer.Ordinal);
        string modulesDirectory = _context.Path("packages", "modules");
        List<string> manifestPaths = [];
        if (Directory.Exists(modulesDirectory))
        {
            foreach (string directory in Directory.GetDirectories(modulesDirectory))
            {
                string candidate = Path.Combine(directory, "resonate.module.toml");
                if (File.Exists(candidate))
                {
                    manifestPaths.Add(candidate);
                }
            }
        }
        manifestPaths.Sort(StringComparer.Ordinal);
        foreach (string manifestPath in manifestPaths)
        {
            Manifest? manifest = Manifest.Read(manifestPath);
            if (manifest is null)
            {
                Problem($"cannot read {Relative(manifestPath)}");
                continue;
            }
            foreach (string problem in manifest.Problems)
            {
                Problem(problem);
            }
            string directory = Path.GetFileName(Path.GetDirectoryName(manifestPath) ?? string.Empty);
            Dictionary<string, object?>? fields = SingleSection(manifest, "resonate.module", directory);
            if (fields is null)
            {
                continue;
            }
            RequireFields(fields, directory, ModuleFields);
            RequireNoExtraFields(fields, directory, ModuleFields);
            ModuleManifest module = new()
            {
                Fields = fields,
                Directory = directory,
                Source = manifestPath,
            };
            modules.Add(module);
            if (module.Id is string id)
            {
                if (byId.ContainsKey(id))
                {
                    Problem($"duplicate module id '{id}'");
                }
                byId[id] = module;
            }
        }

        /* Everything a consumer may legitimately require. */
        Dictionary<string, string> available = new(StringComparer.Ordinal);
        foreach (string capability in TextList(host, "provides"))
        {
            available[capability] = "the host";
            if (!declared.ContainsKey(capability))
            {
                Problem($"host provides '{capability}' but no header declares it");
            }
        }

        foreach (ModuleManifest module in modules)
        {
            foreach (string capability in module.Strings("provides"))
            {
                if (available.TryGetValue(capability, out string? provider))
                {
                    Problem($"{module.Label} provides '{capability}', already provided by {provider}");
                }
                available[capability] = module.Label;
                if (!declared.ContainsKey(capability))
                {
                    Problem($"{module.Label} provides '{capability}' but no header declares it");
                }
            }
            foreach (string capability in module.Strings("requires"))
            {
                if (!available.ContainsKey(capability))
                {
                    Problem($"{module.Label} requires '{capability}', which nothing provides");
                }
                if (!declared.ContainsKey(capability))
                {
                    Problem($"{module.Label} requires '{capability}', which no header declares");
                }
            }
            foreach (string capability in module.Strings("optional"))
            {
                if (!declared.ContainsKey(capability))
                {
                    Problem($"{module.Label} lists '{capability}' as optional but no header declares it");
                }
            }
            foreach (string dependency in module.Strings("depends_on"))
            {
                if (!byId.ContainsKey(dependency))
                {
                    Problem($"{module.Label} depends on '{dependency}', which is not a known module");
                }
            }
        }

        /* Manifest versus build table, both directions. */
        Dictionary<string, BuildEntry> built = new(StringComparer.Ordinal);
        foreach (BuildEntry entry in ParseBuildTable())
        {
            built[entry.Id] = entry;
            if (!byId.TryGetValue(entry.Id, out ModuleManifest? module))
            {
                Problem($"build declares module '{entry.Id}' but no manifest defines it");
            }
            else if (module.Directory != entry.Directory)
            {
                Problem($"'{entry.Id}' is built from '{entry.Directory}' but its manifest is in "
                        + $"'{module.Directory}'");
            }
        }
        foreach (ModuleManifest module in modules)
        {
            if (module.Id is not string id || !built.ContainsKey(id))
            {
                Problem($"{module.Id ?? module.Directory} has a manifest but no entry in "
                        + $"{Relative(Path.Combine(modulesDirectory, "xmake.lua"))}, so nothing "
                        + "builds it");
            }
        }

        /* The descriptor generates min_host_abi from the module's own abi. */
        foreach (ModuleManifest module in modules)
        {
            if (module.Number("min_host_abi") != module.Number("abi"))
            {
                Problem($"{module.Label}: min_host_abi {module.Number("min_host_abi")} but abi "
                        + $"{module.Number("abi")}, and the descriptor derives one from the other");
            }
        }

        foreach (ModuleManifest module in modules)
        {
            if (module.Id is not string id || !built.TryGetValue(id, out BuildEntry? entry))
            {
                continue;
            }
            if (!Sorted(entry.Dependencies).SequenceEqual(Sorted(module.Strings("depends_on"))))
            {
                Problem($"{module.Id}: build links [{Sorted(entry.Dependencies)}] but manifest "
                        + $"depends_on [{Sorted(module.Strings("depends_on"))}]");
            }
        }

        /* Manifest versus descriptor and capability headers. */
        foreach (ModuleManifest module in modules)
        {
            string descriptorPath = Path.Combine(Path.GetDirectoryName(module.Source) ?? string.Empty,
                                                 "src", "module_descriptor.cpp");
            string? text = ReadIfExists(descriptorPath);
            if (text is null)
            {
                Problem($"{module.Label} has no src/module_descriptor.cpp");
                continue;
            }
            Descriptor? descriptor = ParseDescriptor(text);
            if (descriptor is null)
            {
                Problem($"{module.Label} does not define RESONATE_MODULE_DESCRIPTOR");
                continue;
            }
            if (descriptor.Id != module.Id)
            {
                Problem($"{module.Label}: descriptor id '{descriptor.Id}' but manifest says "
                        + $"'{module.Id}'");
            }
            if (descriptor.Name != module.Text("name"))
            {
                Problem($"{module.Label}: descriptor display name '{descriptor.Name}' but manifest "
                        + $"says '{module.Text("name")}'");
            }
            if (descriptor.Version != module.Text("version"))
            {
                Problem($"{module.Label}: descriptor version '{descriptor.Version}' but manifest "
                        + $"says '{module.Text("version")}'");
            }
            if (descriptor.Abi != module.Number("abi"))
            {
                Problem($"{module.Label}: descriptor abi {descriptor.Abi} but manifest says "
                        + $"{module.Number("abi")}");
            }

            string[] keys = ["provides", "requires", "optional"];
            for (int index = 0; index < keys.Length; ++index)
            {
                List<string> expected = [];
                foreach (string capability in module.Strings(keys[index]))
                {
                    if (declared.TryGetValue(capability, out (string Type, string Header) found))
                    {
                        expected.Add(found.Type);
                    }
                }
                List<string> actual = ParseTypeList(descriptor.Lists[index]);
                List<string> missing = Difference(expected, actual);
                List<string> extra = Difference(actual, expected);
                if (missing.Count != 0)
                {
                    Problem($"{module.Label}: descriptor {keys[index]} omits {Sorted(missing)}");
                }
                if (extra.Count != 0)
                {
                    Problem($"{module.Label}: descriptor {keys[index]} lists {Sorted(extra)}, "
                            + "which the manifest does not declare");
                }
            }
            if (!Sorted(ParseNameList(descriptor.Lists[3]))
                     .SequenceEqual(Sorted(module.Strings("depends_on"))))
            {
                Problem($"{module.Label}: descriptor dependencies "
                        + $"[{Sorted(ParseNameList(descriptor.Lists[3]))}] but manifest says "
                        + $"[{Sorted(module.Strings("depends_on"))}]");
            }
        }

        if (_problems.Count != 0)
        {
            foreach (string problem in _problems)
            {
                _context.Output.WriteLine($"  {problem}");
            }
            _context.Error.WriteLine($"module check failed with {_problems.Count} problem(s)");
            return 1;
        }
        _context.Output.WriteLine($"module check: {modules.Count} module(s), {available.Count} "
                                  + "capability provider(s) consistent");
        return 0;
    }

    private void Problem(string message)
    {
        _problems.Add(message);
    }

    private string Relative(string path)
    {
        string relative = Path.GetRelativePath(_context.ProjectDirectory, path);
        return relative.StartsWith("..", StringComparison.Ordinal)
            ? path
            : relative.Replace('\\', '/');
    }

    /* Manifests carry exactly one table, [resonate.<kind>], and every field
       lives inside it. */
    private Dictionary<string, object?>? SingleSection(Manifest manifest, string expected,
                                                      string label)
    {
        if (manifest.Sections.Count != 1 || !manifest.Sections.ContainsKey(expected))
        {
            Problem($"{label}: expected exactly one [{expected}] table");
            return null;
        }
        return manifest.Sections[expected];
    }

    private void RequireFields(Dictionary<string, object?> fields, string label,
                               IReadOnlyList<string> required)
    {
        foreach (string field in required)
        {
            if (!fields.ContainsKey(field))
            {
                Problem($"{label}: manifest has no '{field}'");
            }
        }
    }

    private void RequireNoExtraFields(Dictionary<string, object?> fields, string label,
                                      IReadOnlyList<string> known)
    {
        HashSet<string> names = new(known, StringComparer.Ordinal);
        foreach (string field in fields.Keys)
        {
            if (!names.Contains(field))
            {
                Problem($"{label}: manifest has unknown field '{field}'");
            }
        }
    }

    private static List<string> TextList(Dictionary<string, object?> fields, string field)
    {
        List<string> result = [];
        if (fields.TryGetValue(field, out object? value) && value is List<object?> items)
        {
            foreach (object? item in items)
            {
                if (item is not null)
                {
                    result.Add(item.ToString() ?? string.Empty);
                }
            }
        }
        return result;
    }

    private static string? ReadIfExists(string path)
    {
        try
        {
            return File.Exists(path) ? File.ReadAllText(path) : null;
        }
        catch (IOException)
        {
            return null;
        }
    }

    /* A capability's name is the string its CapabilityTraits::name and ::id are
       both built from, so that is what a manifest has to name. */
    private Dictionary<string, (string Type, string Header)> ScanDeclaredCapabilities()
    {
        Dictionary<string, (string Type, string Header)> byName = new(StringComparer.Ordinal);
        string packages = _context.Path("packages");
        if (!Directory.Exists(packages))
        {
            return byName;
        }
        List<string> headers = Directory.GetFiles(packages, "*.h", SearchOption.AllDirectories)
            .Where(path => path.Replace('\\', '/').Contains("/include/", StringComparison.Ordinal))
            .ToList();
        headers.Sort(StringComparer.Ordinal);
        foreach (string header in headers)
        {
            string? text = ReadIfExists(header);
            if (text is null)
            {
                continue;
            }
            string? currentType = null;
            foreach (string rawLine in text.Split('\n'))
            {
                string line = rawLine.TrimEnd('\r');
                Match type = Regex.Match(line, @"^\s*typedef\s+struct\s+(\w+)\s*\{?\s*$");
                if (!type.Success)
                {
                    type = Regex.Match(line, @"^\s*struct\s+(\w+)\s*$");
                }
                if (type.Success)
                {
                    currentType = type.Groups[1].Value;
                }
                Match name = Regex.Match(line, "const\\s+char\\s*\\*\\s*name\\s*=\\s*\"([^\"]*)\"");
                if (!name.Success)
                {
                    name = Regex.Match(line, "Id\\s+id\\s*\\{\"([^\"]*)\"\\}\\s*;");
                }
                if (name.Success && currentType is not null)
                {
                    byName[name.Groups[1].Value] = (currentType, header);
                }
            }
        }
        return byName;
    }

    /* The text between the parentheses of the macro call, balanced rather than
       assuming it ends the file. */
    private static string? ExtractInvocation(string text, string macro)
    {
        Match match = Regex.Match(text, Regex.Escape(macro) + "\\s*\\(");
        if (!match.Success)
        {
            return null;
        }
        int open = text.IndexOf('(', match.Index, StringComparison.Ordinal);
        int depth = 0;
        bool inString = false;
        for (int index = open; index < text.Length; ++index)
        {
            char character = text[index];
            if (inString)
            {
                if (character == '"')
                {
                    inString = false;
                }
            }
            else if (character == '"')
            {
                inString = true;
            }
            else if (character == '(')
            {
                ++depth;
            }
            else if (character == ')')
            {
                --depth;
                if (depth == 0)
                {
                    return text[(open + 1)..index];
                }
            }
        }
        return null;
    }

    private static List<string> SplitTopLevel(string invocation)
    {
        List<string> args = [];
        int start = 0;
        int angle = 0;
        int paren = 0;
        int brace = 0;
        bool inString = false;
        for (int index = 0; index < invocation.Length; ++index)
        {
            char character = invocation[index];
            if (inString)
            {
                if (character == '"')
                {
                    inString = false;
                }
            }
            else if (character == '"')
            {
                inString = true;
            }
            else if (character == '<')
            {
                ++angle;
            }
            else if (character == '>')
            {
                --angle;
            }
            else if (character == '(')
            {
                ++paren;
            }
            else if (character == ')')
            {
                --paren;
            }
            else if (character == '{')
            {
                ++brace;
            }
            else if (character == '}')
            {
                --brace;
            }
            else if (character == ',' && angle == 0 && paren == 0 && brace == 0)
            {
                args.Add(invocation[start..index]);
                start = index + 1;
            }
        }
        args.Add(invocation[start..]);
        return args;
    }

    private static Descriptor? ParseDescriptor(string text)
    {
        string? invocation = ExtractInvocation(text, "RESONATE_MODULE_DESCRIPTOR");
        if (invocation is null)
        {
            return null;
        }
        List<string> args = SplitTopLevel(invocation);
        string? StringArg(int index)
        {
            if (index >= args.Count)
            {
                return null;
            }
            Match match = Regex.Match(args[index], "^\\s*\"([^\"]*)\"");
            return match.Success ? match.Groups[1].Value : null;
        }
        long abi = 0;
        if (args.Count > 3)
        {
            Match digits = Regex.Match(args[3], "\\d+");
            if (digits.Success)
            {
                abi = long.Parse(digits.Value);
            }
        }
        List<string?> lists = [];
        for (int index = 4; index < 8; ++index)
        {
            lists.Add(index < args.Count ? args[index] : null);
        }
        return new Descriptor(StringArg(0), StringArg(1), StringArg(2), abi, lists);
    }

    private static List<string> ParseTypeList(string? list)
    {
        List<string> types = [];
        if (list is null)
        {
            return types;
        }
        Match match = Regex.Match(list, "<(.+)>", RegexOptions.Singleline);
        if (!match.Success)
        {
            match = Regex.Match(list, "\\((.*?)\\)\\s*$", RegexOptions.Singleline);
        }
        if (!match.Success)
        {
            return types;
        }
        foreach (Match type in Regex.Matches(match.Groups[1].Value, "\\w+"))
        {
            types.Add(type.Value);
        }
        return types;
    }

    private static List<string> ParseNameList(string? list)
    {
        List<string> names = [];
        if (list is null)
        {
            return names;
        }
        foreach (Match match in Regex.Matches(list, "\"([^\"]*)\""))
        {
            names.Add(match.Groups[1].Value);
        }
        return names;
    }

    /* The module table in packages/modules/xmake.lua, kept one entry per line:
       { dir = "dir", id = "id", deps = {"id", ...} }. */
    private List<BuildEntry> ParseBuildTable()
    {
        string path = _context.Path("packages", "modules", "xmake.lua");
        string? text = ReadIfExists(path);
        if (text is null)
        {
            Problem($"cannot read {Relative(path)}");
            return [];
        }
        List<BuildEntry> entries = [];
        foreach (string rawLine in text.Split('\n'))
        {
            Match match = Regex.Match(rawLine,
                "\\{\\s*dir\\s*=\\s*\"([\\w-]+)\"\\s*,\\s*id\\s*=\\s*\"([\\w.-]+)\"\\s*,"
                + "\\s*deps\\s*=\\s*\\{(.*)\\}\\s*\\}");
            if (!match.Success)
            {
                continue;
            }
            List<string> dependencies = [];
            foreach (Match dependency in Regex.Matches(match.Groups[3].Value, "\"([\\w.-]+)\""))
            {
                dependencies.Add(dependency.Groups[1].Value);
            }
            entries.Add(new BuildEntry(match.Groups[1].Value, match.Groups[2].Value, dependencies));
        }
        if (entries.Count == 0)
        {
            Problem($"no module entries found in {Relative(path)}");
        }
        return entries;
    }

    private static string Sorted(IEnumerable<string> values)
    {
        return string.Join(", ", values.OrderBy(value => value, StringComparer.Ordinal));
    }

    private static List<string> Difference(List<string> left, List<string> right)
    {
        HashSet<string> lookup = new(right, StringComparer.Ordinal);
        return left.Where(value => !lookup.Contains(value)).ToList();
    }

    private sealed record BuildEntry(string Directory, string Id, List<string> Dependencies);

    private sealed record Descriptor(string? Id, string? Name, string? Version, long Abi,
                                     List<string?> Lists);
}
