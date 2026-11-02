using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Resonate.Builder.Packaging;

/* The products manifest's TOML subset, kept deliberately narrow: `[[product]]`
   blocks whose values are quoted strings or integers, comments and blank
   lines. This is the machine-written shape assetindex and texcook emit; a
   hand-written manifest outside the subset fails here rather than staging
   half of it. */
internal sealed class ProductsManifest
{
    internal sealed class Product
    {
        public required string Guid { get; init; }
        public required string Source { get; init; }
        public string Importer { get; init; } = string.Empty;
        public required string Fingerprint { get; init; }
        public required long SubId { get; init; }
        public required string Path { get; init; }
    }

    public List<Product> Products { get; } = [];

    public static ProductsManifest Read(string path)
    {
        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (IOException error)
        {
            throw new BuilderException($"cannot read {path}: {error.Message}");
        }
        ProductsManifest manifest = new();
        manifest.Parse(text, path);
        return manifest;
    }

    private void Parse(string text, string file)
    {
        Dictionary<string, string>? fields = null;
        int line = 0;
        foreach (string raw in text.Split('\n'))
        {
            ++line;
            string content = raw.TrimEnd('\r').Trim();
            if (content.Length == 0 || content.StartsWith('#'))
            {
                continue;
            }
            if (content == "[[product]]")
            {
                fields = [];
                continue;
            }
            if (content.StartsWith('['))
            {
                throw new BuilderException($"{file}:{line}: unexpected table header '{content}'");
            }
            int equals = content.IndexOf('=');
            if (equals <= 0)
            {
                throw new BuilderException($"{file}:{line}: expected 'key = value'");
            }
            string key = content[..equals].Trim();
            string value = content[(equals + 1)..].Trim();
            if (fields == null)
            {
                throw new BuilderException($"{file}:{line}: '{key}' outside a [[product]] block");
            }
            fields[key] = value;
            if (key == "path")
            {
                Flush(file, line, fields);
                fields = null;
            }
        }
        if (fields is { Count: > 0 })
        {
            throw new BuilderException($"{file}: a [[product]] block has no path");
        }
    }

    /* `path` closes every block the writers emit; requiring it keeps a half
       parsed product out of the staging manifest. */
    private void Flush(string file, int line, Dictionary<string, string> fields)
    {
        string guid = Take(fields, "guid", file, line);
        string source = Take(fields, "source", file, line);
        string fingerprint = Take(fields, "fingerprint", file, line);
        string path = Take(fields, "path", file, line);
        if (!fields.TryGetValue("sub_id", out string? raw_sub) ||
            !long.TryParse(raw_sub, out long sub_id) || sub_id <= 0 ||
            sub_id > uint.MaxValue)
        {
            throw new BuilderException($"{file}:{line}: sub_id must be a positive 32-bit integer");
        }
        fields.TryGetValue("importer", out string? importer);
        Products.Add(new Product
        {
            Guid = Unquote(guid),
            Source = Unquote(source),
            Importer = importer is null ? string.Empty : Unquote(importer),
            Fingerprint = Unquote(fingerprint),
            SubId = sub_id,
            Path = Unquote(path),
        });
    }

    private static string Take(Dictionary<string, string> fields, string key, string file, int line)
    {
        if (!fields.TryGetValue(key, out string? value))
        {
            throw new BuilderException($"{file}:{line}: a [[product]] block has no {key}");
        }
        return value;
    }

    private static string Unquote(string value)
    {
        if (value.Length < 2 || value[0] != '"' || value[^1] != '"')
        {
            return value;
        }
        return value[1..^1].Replace("\\\\", "\\", StringComparison.Ordinal)
            .Replace("\\\"", "\"", StringComparison.Ordinal)
            .Replace("\\n", "\n", StringComparison.Ordinal);
    }
}
