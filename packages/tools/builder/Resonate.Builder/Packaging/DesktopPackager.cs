using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Resonate.Builder.Packaging;

/* Assembles a shipped desktop tree: the executable, its plugin libraries and
   manifests, the shaders, the cooked content, the texture cook's products and
   the staged asset manifest. The layout is what the runtime's own walks find:
   content beside the executable, plugins named through --plugin-dir. */
internal sealed class DesktopPackager
{
    private readonly BuildContext _context;
    private readonly string _exe;
    private readonly string _plugins;
    private readonly string _staging;

    internal DesktopPackager(BuildContext context, string exe, string plugins, string outDirectory)
    {
        _context = context;
        _exe = exe;
        _plugins = plugins;
        _staging = outDirectory;
    }

    internal void Run()
    {
        if (!File.Exists(_exe))
        {
            throw new BuilderException($"the executable is missing: {_exe}; build it first");
        }
        if (Directory.Exists(_staging))
        {
            Directory.Delete(_staging, recursive: true);
        }
        Directory.CreateDirectory(Path.Combine(_staging, "plugins"));
        Copy(_exe, Path.Combine(_staging, Path.GetFileName(_exe)));

        /* Plugins: the module libraries and the manifest of the same id beside
           each, which is what the loader pairs. A library no manifest names is
           a link-time dependency (the UI's tgfx wrapper); the loader would
           only probe and reject it, and the executable needs it beside
           itself. The exe's own directory holds the ones the default target
           dir produced. */
        List<string> manifests = Files(_plugins, "*.toml");
        foreach (string manifest in manifests)
        {
            Copy(manifest, Path.Combine(_staging, "plugins", Path.GetFileName(manifest)));
        }
        List<string> libraries =
            Files(_plugins, "*.dll").Concat(Files(_plugins, "*.so"))
                .Concat(Files(_plugins, "*.dylib"))
                .Concat(Files(Path.GetDirectoryName(_exe)!, "*.dll"))
                .ToList();
        foreach (string library in libraries)
        {
            string name = Path.GetFileName(library);
            string? module = manifests.Find(
                manifest => Path.GetFileNameWithoutExtension(name)
                    .EndsWith(Path.GetFileNameWithoutExtension(manifest),
                              StringComparison.OrdinalIgnoreCase));
            if (module != null)
            {
                Copy(library, Path.Combine(_staging, "plugins", name));
            }
            else
            {
                Copy(library, Path.Combine(_staging, name));
            }
        }

        string shaders = _context.Path("build", "shaders");
        foreach (string shader in Files(shaders, "*.spv"))
        {
            Copy(shader, Path.Combine(_staging, "shaders", Path.GetFileName(shader)));
        }

        /* The cooked tree ships wholesale: it is the tree the cook verified. */
        string cooked = _context.Path("build", "cooked");
        if (!Directory.Exists(cooked))
        {
            throw new BuilderException(
                $"no cooked content under {cooked}; run `xmake cook` first");
        }
        foreach (string file in Directory.EnumerateFiles(cooked, "*", SearchOption.AllDirectories))
        {
            string relative = Path.GetRelativePath(cooked, file);
            Copy(file, Path.Combine(_staging, "content", relative));
        }

        /* The texture cook's manifest names its products by bare file name, so
           it travels beside them unchanged. */
        string textures = _context.Path("build", "content", "textures");
        string texture_manifest = Path.Combine(textures, "products.toml");
        if (File.Exists(texture_manifest))
        {
            Copy(texture_manifest, Path.Combine(_staging, "content", "textures", "products.toml"));
            foreach (string texture in Files(textures, "*.ktx2"))
            {
                Copy(texture,
                     Path.Combine(_staging, "content", "textures", Path.GetFileName(texture)));
            }
        }
        else
        {
            _context.Output.WriteLine("desktop: no texture manifest; the run serves no textures");
        }

        int products = StagedProducts.Write(_context, Path.Combine(_staging, "content", "animation"),
                                            "products.toml", null);
        _context.Output.WriteLine(
            products != 0
                ? $"desktop: {_staging} ({products} asset product(s))"
                : $"desktop: {_staging}");
    }

    private static List<string> Files(string directory, string pattern)
    {
        if (!Directory.Exists(directory))
        {
            return [];
        }
        List<string> files = new(Directory.GetFiles(directory, pattern));
        files.Sort(StringComparer.Ordinal);
        return files;
    }

    private static void Copy(string source, string target)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Copy(source, target, overwrite: true);
    }
}
