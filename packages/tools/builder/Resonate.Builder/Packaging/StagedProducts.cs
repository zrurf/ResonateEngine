using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Resonate.Builder.Hashing;

namespace Resonate.Builder.Packaging;

/* Writes the products manifest a packaged build ships. The development
   manifest's paths reach back into the source tree; a package ships the cooked
   products flat, so it writes its own manifest beside them. The identity
   (guid, sub id, importer) is the source's; the fingerprint is recomputed over
   the bytes that actually ship. Only products whose file sits in the staged
   directory are listed. */
internal static class StagedProducts
{
    /* Writes `<directory>/<name>` listing every product file found there.
       Answers how many products were listed; zero also means no file. */
    internal static int Write(BuildContext context, string directory, string name,
                              HashSet<string>? staged)
    {
        string dev = context.Path("build", "content", "products.toml");
        if (!File.Exists(dev))
        {
            return 0;
        }
        staged ??= Directory.Exists(directory)
            ? Directory.EnumerateFiles(directory)
                .Select(file => Path.GetFileName(file))
                .ToHashSet(StringComparer.Ordinal)
            : [];
        StringBuilder text = new();
        text.Append("# Written by the packager; the runtime's asset identity reads it.\n");
        int count = 0;
        foreach (ProductsManifest.Product product in ProductsManifest.Read(dev).Products)
        {
            string file = Path.GetFileName(product.Path.Replace('\\', '/'));
            if (!staged.Contains(file))
            {
                continue;
            }
            byte[] bytes = File.ReadAllBytes(Path.Combine(directory, file));
            text.Append("[[product]]\n");
            text.Append("source = \"").Append(Escape(product.Source)).Append("\"\n");
            text.Append("guid = \"").Append(product.Guid).Append("\"\n");
            if (product.Importer.Length != 0)
            {
                text.Append("importer = \"").Append(Escape(product.Importer)).Append("\"\n");
            }
            text.Append("fingerprint = \"")
                .Append(Convert.ToHexStringLower(Blake3.Hash(bytes)))
                .Append("\"\n");
            text.Append("sub_id = ").Append(product.SubId).Append('\n');
            text.Append("path = \"").Append(Escape(file)).Append("\"\n\n");
            ++count;
        }
        if (count == 0)
        {
            return 0;
        }
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, name), text.ToString());
        return count;
    }

    private static string Escape(string text)
    {
        return text.Replace("\\", "\\\\", StringComparison.Ordinal)
            .Replace("\"", "\\\"", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal);
    }
}
