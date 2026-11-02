using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Resonate.Builder.Assets;
using Resonate.Builder.Hashing;

namespace Resonate.Builder.Commands;

/* Indexes the content tree's identity sidecars into one products manifest.
   Unlike the texture cook this writes no products: a .meta beside a source
   *is* the product, with sub id 1, and the manifest makes the id addressable.
   Only sources that already carry a sidecar are indexed -- identity is a
   deliberate authoring act, not something a walk hands out. */
internal static class AssetsCommand
{
    private sealed class Entry
    {
        public required string Source { get; init; }
        public required string Guid { get; init; }
        public required string Importer { get; init; }
        public required string Fingerprint { get; init; }
        public required string Product { get; init; }
    }

    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate assets: unknown argument '{args[0]}'");
            return 2;
        }
        string content = context.Path("content");
        string textures = Path.Combine(content, "render", "textures");
        string outDirectory = context.BuildPath("content");
        List<string> metas = EnumerateMetas(content);
        List<Entry> entries = [];
        Dictionary<string, string> byGuid = new(StringComparer.Ordinal);
        foreach (string metaPath in metas)
        {
            string source = metaPath[..^".meta".Length];
            if (!File.Exists(source))
            {
                context.Error.WriteLine($"assetindex: {metaPath}: the sidecar has no source beside it");
                continue;
            }
            if (IsInside(source, textures))
            {
                continue; /* texcook's manifest owns the textures. */
            }
            if (!AssetMeta.Read(metaPath, out AssetMeta meta, out string error))
            {
                context.Error.WriteLine($"assetindex: {metaPath}: {error}");
                return 1;
            }
            string guid = AssetMeta.FormatGuid(meta.GuidHigh, meta.GuidLow);
            if (byGuid.TryGetValue(guid, out string? existing))
            {
                context.Error.WriteLine($"assetindex: {existing} and {source} share the guid {guid}");
                return 1;
            }
            byGuid[guid] = source;
            byte[] bytes = File.ReadAllBytes(source);
            string fingerprint = Convert.ToHexStringLower(Blake3.Hash(bytes));
            if (meta.SourceHash.Length != 0 && meta.SourceHash != fingerprint)
            {
                context.Error.WriteLine(
                    $"assetindex: {source} changed since the importer last hashed it");
            }
            entries.Add(new Entry
            {
                Source = Relative(source, content),
                Guid = guid,
                Importer = meta.Importer,
                Fingerprint = fingerprint,
                Product = Relative(source, outDirectory),
            });
        }
        entries.Sort((left, right) => string.CompareOrdinal(left.Source, right.Source));

        Directory.CreateDirectory(outDirectory);
        string manifest = Path.Combine(outDirectory, "products.toml");
        StringBuilder text = new();
        text.Append("# Written by assetindex; the runtime's asset identity reads it.\n");
        foreach (Entry entry in entries)
        {
            text.Append("[[product]]\n");
            text.Append("source = \"").Append(Escape(entry.Source)).Append("\"\n");
            text.Append("guid = \"").Append(entry.Guid).Append("\"\n");
            if (entry.Importer.Length != 0)
            {
                text.Append("importer = \"").Append(Escape(entry.Importer)).Append("\"\n");
            }
            text.Append("fingerprint = \"").Append(entry.Fingerprint).Append("\"\n");
            text.Append("sub_id = 1\n");
            text.Append("path = \"").Append(Escape(entry.Product)).Append("\"\n\n");
        }
        try
        {
            File.WriteAllText(manifest, text.ToString());
        }
        catch (IOException error)
        {
            context.Error.WriteLine($"assetindex: {manifest}: {error.Message}");
            return 1;
        }
        context.Output.WriteLine($"assetindex: {entries.Count} source(s) -> {manifest}");
        return 0;
    }

    private static List<string> EnumerateMetas(string root)
    {
        if (!Directory.Exists(root))
        {
            return [];
        }
        List<string> metas = Directory.GetFiles(root, "*.meta", SearchOption.AllDirectories).ToList();
        metas.Sort(StringComparer.Ordinal);
        return metas;
    }

    private static bool IsInside(string path, string directory)
    {
        string prefix = directory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                        + Path.DirectorySeparatorChar;
        return path.StartsWith(prefix, StringComparison.Ordinal);
    }

    /* Forward slashes on every platform: the manifest is read where paths are
       compared as text, and a Windows separator would not resolve elsewhere. */
    private static string Relative(string path, string basePath)
    {
        return Path.GetRelativePath(basePath, path).Replace('\\', '/');
    }

    private static string Escape(string text)
    {
        return text.Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("\"", "\\\"", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal);
    }
}
