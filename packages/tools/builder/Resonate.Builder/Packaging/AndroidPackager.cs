using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using Resonate.Builder.Hashing;

namespace Resonate.Builder.Packaging;

/* Packages the built Android host, its plugin libraries, their manifests,
   shaders and cooked content into a signed APK. The SDK's own tools do the
   format work; the assembler only orders them. */
internal sealed class AndroidPackager
{
    private readonly BuildContext _context;
    private readonly string _host;
    private readonly string _plugins;
    private readonly string _abi;
    private readonly string _ndk;
    private readonly string _sdk;
    private readonly string _sdkVersion;
    private readonly string _staging;

    internal AndroidPackager(BuildContext context, string host, string plugins, string abi,
                             string ndk, string sdk, string sdkVersion)
    {
        _context = context;
        _host = host;
        _plugins = plugins;
        _abi = abi;
        _ndk = ndk;
        _sdk = sdk;
        _sdkVersion = sdkVersion;
        _staging = context.BuildPath("apk");
    }

    internal void Run()
    {
        if (!File.Exists(_host))
        {
            throw new BuilderException($"the Android host is missing: {_host}; build it first");
        }
        string buildTools = NewestBuildTools(Path.Combine(_sdk, "build-tools"))
                            ?? throw new BuilderException(
                                $"no Android build-tools under {Path.Combine(_sdk, "build-tools")}");
        bool windows = OperatingSystem.IsWindows();
        string tool = windows ? ".exe" : string.Empty;
        string aapt2 = Path.Combine(buildTools, "aapt2" + tool);
        string zipalign = Path.Combine(buildTools, "zipalign" + tool);
        string apksigner = Path.Combine(buildTools, "lib", "apksigner.jar");
        string d8 = Path.Combine(buildTools, "d8" + (windows ? ".bat" : string.Empty));

        (string platform, int platformVersion) = NewestPlatform(Path.Combine(_sdk, "platforms"))
            ?? throw new BuilderException(
                $"no Android platform under {Path.Combine(_sdk, "platforms")}");
        string androidJar = Path.Combine(platform, "android.jar");

        if (Directory.Exists(_staging))
        {
            Directory.Delete(_staging, recursive: true);
        }
        string libraryDirectory = Path.Combine(_staging, "lib", _abi);
        Directory.CreateDirectory(libraryDirectory);
        Directory.CreateDirectory(Path.Combine(_staging, "assets", "plugins"));

        Copy(_host, Path.Combine(libraryDirectory, Path.GetFileName(_host)));
        foreach (string library in Files(_plugins, "*.so"))
        {
            Copy(library, Path.Combine(libraryDirectory, Path.GetFileName(library)));
        }

        /* libtgfx.so lives in its own CMake build tree; the wrapper .so beside
           the plugins names it as a dependency, so it travels in the same ABI
           directory. tgfx builds against the NDK's shared C++ runtime, so that
           runtime travels with it. */
        string tgfx = _context.Path("build", "tgfx", "android", "release", "libtgfx.so");
        if (File.Exists(tgfx))
        {
            Copy(tgfx, Path.Combine(libraryDirectory, "libtgfx.so"));
            string? cxxShared = FindCxxShared();
            if (cxxShared is not null)
            {
                Copy(cxxShared, Path.Combine(libraryDirectory, "libc++_shared.so"));
            }
        }

        /* The loader pairs a library with a manifest of the same id; the baked
           manifests travel as assets because the APK's library directory is
           read-only, and no text form ships. */
        foreach (string directory in Directory.GetDirectories(_context.Path("packages", "modules")))
        {
            string manifest = Path.Combine(directory, "resonate.module.toml");
            if (!File.Exists(manifest))
            {
                continue;
            }
            Match id = Regex.Match(File.ReadAllText(manifest), "id\\s*=\\s*\"([^\"]+)\"");
            if (id.Success)
            {
                Bake(manifest,
                     Path.Combine(_staging, "assets", "plugins", id.Groups[1].Value + ".rman"));
            }
        }

        StageShaders();
        StageContent();
        StageTextures();
        StageAssetManifest();
        StageJava(androidJar, d8);

        string resources = Path.Combine(_staging, "resources.zip");
        string baseApk = Path.Combine(_staging, "base.apk");
        string aligned = Path.Combine(_staging, "aligned.apk");
        string output = _context.BuildPath("resonate.apk");
        Run(aapt2, "compile", "--dir", _context.Path("packages", "host", "android", "res"),
            "-o", resources);
        Run(aapt2, "link", "-o", baseApk,
            "--manifest", _context.Path("packages", "host", "android", "AndroidManifest.xml"),
            "-I", androidJar,
            "--min-sdk-version", _sdkVersion,
            "--target-sdk-version", platformVersion.ToString(),
            "--version-code", "1", "--version-name", "0.1", resources);
        /* jar rather than aapt2 -A: aapt2 on Windows writes the staged assets
           with backslashes in their entry names, which the asset manager looks
           up with forward slashes. */
        Run("jar", "--update", "--file", baseApk, "-C", _staging, "lib", "-C", _staging,
            "assets");
        string dex = Path.Combine(_staging, "dex", "classes.dex");
        if (File.Exists(dex))
        {
            Run("jar", "--update", "--file", baseApk, "-C", Path.GetDirectoryName(dex)!, "classes.dex");
        }
        Run(zipalign, "-f", "-p", "4", baseApk, aligned);

        string keystore = _context.BuildPath("debug.keystore");
        if (!File.Exists(keystore))
        {
            Run("keytool", "-genkeypair", "-keystore", keystore, "-storepass", "android",
                "-keypass", "android", "-alias", "androiddebugkey", "-keyalg", "RSA", "-keysize",
                "2048", "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US");
        }
        /* A small heap: apksigner needs little, and the default reservation
           fails on machines with a tight commit limit. */
        Run("java", "-Xmx256m", "-Xss1m", "-jar", apksigner, "sign", "--ks", keystore,
            "--ks-key-alias", "androiddebugkey", "--ks-pass", "pass:android", "--key-pass",
            "pass:android", "--out", output, aligned);
        _context.Output.WriteLine($"android: {output}");
    }

    private void StageShaders()
    {
        List<string> shaders = Files(_context.Path("build", "shaders"), "*.spv");
        if (shaders.Count == 0)
        {
            throw new BuilderException(
                $"no built shaders under {_context.Path("build", "shaders")}; run `xmake slang` first");
        }
        string target = Path.Combine(_staging, "assets", "shaders");
        Directory.CreateDirectory(target);
        foreach (string shader in shaders)
        {
            Copy(shader, Path.Combine(target, Path.GetFileName(shader)));
        }
    }

    /* Assets ship cooked: the source documents are XML and the package carries
       the binary codec's form, which is also what keeps the authored text out
       of the APK. The cooked tree is platform-independent, so one host-side
       cook serves every target. */
    private void StageContent()
    {
        string cooked = _context.Path("build", "cooked");
        if (!Directory.Exists(cooked))
        {
            throw new BuilderException(
                $"no cooked content under {cooked}; run `xmake cook` first");
        }
        /* The cook rewrites the whole tree, so its newest file is the cook's
           time; a source edited after that would ship stale. */
        DateTime cookedNewest = NewestWriteTime(cooked);
        DateTime sourceNewest = NewestWriteTime(_context.Path("content"));
        if (sourceNewest > cookedNewest)
        {
            throw new BuilderException(
                $"content changed after the last cook ({sourceNewest:HH:mm} > "
                + $"{cookedNewest:HH:mm}); run `xmake cook` first");
        }
        StageFiles(Path.Combine(cooked, "ui"), "*.rui", "ui");
        StageFiles(Path.Combine(cooked, "ui"), "*.rcss", "ui");
        StageFiles(Path.Combine(cooked, "ui"), "*.rstrings", "ui");
        StageFiles(Path.Combine(cooked, "ui", "fonts"), "*.ttf", "fonts");
        StageFiles(Path.Combine(cooked, "ui", "images"), "*.png", "images");
        StageFiles(Path.Combine(cooked, "ui"), "*.icons", "ui");
        StageFiles(Path.Combine(cooked, "scene"), "*.rscene", "scene");
        StageFiles(Path.Combine(cooked, "animation"), "*.rclip", "animation");
    }

    private static DateTime NewestWriteTime(string directory)
    {
        DateTime newest = DateTime.MinValue;
        if (!Directory.Exists(directory))
        {
            return newest;
        }
        foreach (string file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories))
        {
            DateTime write = File.GetLastWriteTimeUtc(file);
            if (write > newest)
            {
                newest = write;
            }
        }
        return newest;
    }

    private void StageFiles(string source, string pattern, string assetDirectory)
    {
        List<string> files = Files(source, pattern);
        if (files.Count == 0)
        {
            return;
        }
        string target = Path.Combine(_staging, "assets", assetDirectory);
        Directory.CreateDirectory(target);
        foreach (string file in files)
        {
            Copy(file, Path.Combine(target, Path.GetFileName(file)));
        }
    }

    /* The texture cook's manifest names its products by bare file name, so it
       travels beside them unchanged. */
    private void StageTextures()
    {
        string textures = _context.Path("build", "content", "textures");
        string manifest = Path.Combine(textures, "products.toml");
        if (!File.Exists(manifest))
        {
            _context.Output.WriteLine("android: no texture manifest; the run serves no textures");
            return;
        }
        string target = Path.Combine(_staging, "assets", "textures");
        Directory.CreateDirectory(target);
        Bake(manifest, Path.Combine(target, "products.rman"));
        foreach (string texture in Files(textures, "*.ktx2"))
        {
            Copy(texture, Path.Combine(target, Path.GetFileName(texture)));
        }
    }

    /* The package ships the cooked products flat, so it writes its own
       manifest beside them (StagedProducts) and bakes it; only the clips are
       asset products today. */
    private void StageAssetManifest()
    {
        int count = StagedProducts.Write(_context, Path.Combine(_staging, "assets", "animation"),
                                         "products.toml", null);
        string manifest = Path.Combine(_staging, "assets", "animation", "products.toml");
        if (File.Exists(manifest))
        {
            Bake(manifest, Path.Combine(_staging, "assets", "animation", "products.rman"));
            File.Delete(manifest);
        }
        if (count != 0)
        {
            _context.Output.WriteLine($"android: {count} asset product(s) staged");
        }
    }

    /* The Java activity owns the IME input connection (Android delivers
       composed text to a View, not to the native window). There is no Gradle
       here, so javac compiles the sources against android.jar and d8 turns the
       classes into the single classes.dex the package carries. */
    private void StageJava(string androidJar, string d8)
    {
        List<string> java = FilesRecursive(_context.Path("packages", "host", "android", "java"),
                                           "*.java");
        if (java.Count == 0)
        {
            return;
        }
        string classes = Path.Combine(_staging, "classes");
        string dex = Path.Combine(_staging, "dex");
        Directory.CreateDirectory(classes);
        Directory.CreateDirectory(dex);
        List<string> javac = ["-source", "8", "-target", "8", "-nowarn", "-classpath", androidJar,
                              "-d", classes];
        javac.AddRange(java);
        Run("javac", javac.ToArray());
        List<string> d8Arguments = ["--lib", androidJar, "--min-api", _sdkVersion, "--output", dex];
        foreach (string file in FilesRecursive(classes, "*.class"))
        {
            d8Arguments.Add(file);
        }
        Run(d8, d8Arguments.ToArray());
    }

    private string? FindCxxShared()
    {
        string toolchains = Path.Combine(_ndk, "toolchains", "llvm", "prebuilt");
        if (!Directory.Exists(toolchains))
        {
            return null;
        }
        string triple = _abi switch
        {
            "arm64-v8a" => "aarch64-linux-android",
            "armeabi-v7a" => "armv7a-linux-androideabi",
            "x86_64" => "x86_64-linux-android",
            "x86" => "i686-linux-android",
            _ => "aarch64-linux-android",
        };
        foreach (string host in Directory.GetDirectories(toolchains))
        {
            string candidate = Path.Combine(host, "sysroot", "usr", "lib", triple,
                                            "libc++_shared.so");
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }
        return null;
    }

    private static string? NewestBuildTools(string root)
    {
        if (!Directory.Exists(root))
        {
            return null;
        }
        string? best = null;
        long bestKey = -1;
        foreach (string directory in Directory.GetDirectories(root))
        {
            Match match = Regex.Match(Path.GetFileName(directory), "^(\\d+)\\.(\\d+)\\.?(\\d*)");
            if (!match.Success)
            {
                continue;
            }
            long key = long.Parse(match.Groups[1].Value) * 1000000
                       + long.Parse(match.Groups[2].Value) * 1000
                       + (match.Groups[3].Value.Length == 0 ? 0 : long.Parse(match.Groups[3].Value));
            if (key > bestKey)
            {
                best = directory;
                bestKey = key;
            }
        }
        return best;
    }

    private static (string Path, int Version)? NewestPlatform(string root)
    {
        if (!Directory.Exists(root))
        {
            return null;
        }
        string? best = null;
        int bestVersion = -1;
        foreach (string directory in Directory.GetDirectories(root, "android-*"))
        {
            Match match = Regex.Match(Path.GetFileName(directory), "android-(\\d+)");
            if (!match.Success)
            {
                continue;
            }
            int version = int.Parse(match.Groups[1].Value);
            if (version > bestVersion)
            {
                best = directory;
                bestVersion = version;
            }
        }
        return best is null ? null : (best, bestVersion);
    }

    private static List<string> Files(string directory, string pattern)
    {
        if (!Directory.Exists(directory))
        {
            return [];
        }
        List<string> files = Directory.GetFiles(directory, pattern).ToList();
        files.Sort(StringComparer.Ordinal);
        return files;
    }

    private static List<string> FilesRecursive(string directory, string pattern)
    {
        if (!Directory.Exists(directory))
        {
            return [];
        }
        List<string> files = Directory.GetFiles(directory, pattern, SearchOption.AllDirectories)
            .ToList();
        files.Sort(StringComparer.Ordinal);
        return files;
    }

    private static void Copy(string source, string target)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Copy(source, target, overwrite: true);
    }

    private static void Run(string program, params string[] arguments)
    {
        ProcessRunner.MustRun(program, arguments);
    }

    /* Bakes one staged manifest through the host tool the packaging tasks
       build first; a shipped build never carries the text form. */
    private void Bake(string source, string target)
    {
        /* Host tools always land in the project's build/tools, whichever
           platform the packaging config targets. */
        string tool = Path.Combine(_context.ProjectDirectory, "build", "tools",
                                   "Resonate.Tools.ManifestBake" +
                                   (OperatingSystem.IsWindows() ? ".exe" : string.Empty));
        if (!File.Exists(tool))
        {
            throw new BuilderException($"the manifest bake tool is missing: {tool}");
        }
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        Run(tool, source, target);
    }
}
