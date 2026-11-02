using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Resonate.Builder.Commands;

/* Fetches and builds tgfx with its Vulkan backend. tgfx and its vendored
   dependencies are a build artifact: the checkout and the CMake build tree both
   live under build/, so neither is committed. Windows produces
   build/tgfx/<mode>/tgfx.lib through the MSVC toolchain; Android produces
   build/tgfx/android/<mode>/libtgfx.so through the NDK toolchain file. */
internal static class TgfCommand
{
    private const string Repository = "https://github.com/Tencent/tgfx.git";

    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        string? platform = null;
        string? mode = null;
        string? ndk = null;
        string arch = "arm64-v8a";
        int sdkVersion = 30;
        for (int index = 0; index < args.Count; ++index)
        {
            switch (args[index])
            {
                case "--platform" when index + 1 < args.Count:
                    platform = args[++index];
                    break;
                case "--mode" when index + 1 < args.Count:
                    mode = args[++index];
                    break;
                case "--ndk" when index + 1 < args.Count:
                    ndk = args[++index];
                    break;
                case "--arch" when index + 1 < args.Count:
                    arch = args[++index];
                    break;
                case "--sdk-version" when index + 1 < args.Count:
                    sdkVersion = int.Parse(args[++index]);
                    break;
                default:
                    context.Error.WriteLine($"resonate tgfx: unknown argument '{args[index]}'");
                    return 2;
            }
        }
        if (platform is null)
        {
            context.Error.WriteLine("resonate tgfx: --platform is required");
            return 2;
        }
        bool android = platform == "android";
        mode ??= "debug";
        string configuration = mode == "release" ? "Release" : "Debug";
        string source = context.Path("build", "third_party", "tgfx");

        try
        {
            if (!Directory.Exists(Path.Combine(source, "include", "tgfx")))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(source)!);
                ProcessRunner.MustRun("git", ["clone", "--depth", "1", Repository, source],
                    new Dictionary<string, string> { ["GIT_LFS_SKIP_SMUDGE"] = "1" });
            }
            if (!File.Exists(Path.Combine(source, "third_party", "vendor_tools", "vendor.cmake")))
            {
                if (ProcessRunner.Run("depsync", []) != 0)
                {
                    throw new BuilderException(
                        "tgfx needs its vendored dependencies: npm install -g depsync, then "
                        + "xmake tgfx");
                }
            }

            /* tgfx passes a clang-only warning flag when compiling volk, which
               MSVC rejects. The guard is upstream-safe. */
            Patch(source, "    set_source_files_properties(${VOLK_FILES} PROPERTIES "
                          + "COMPILE_OPTIONS -Wno-misleading-indentation)",
                  "    if (NOT MSVC)\n"
                  + "    set_source_files_properties(${VOLK_FILES} PROPERTIES COMPILE_OPTIONS "
                  + "-Wno-misleading-indentation)\n    endif ()",
                  "if (NOT MSVC)\n    set_source_files_properties(${VOLK_FILES}");

            /* On Android the archive is built shared: the static archive
               exceeds the Windows command line for CMake's archive rule and the
               vendor merge tool, while a shared library links through a
               response file. The merge assumes a static input, so the vendor
               archives link into the .so instead, and the soname must be
               libtgfx.so. */
            if (android)
            {
                Patch(source, "add_library(tgfx STATIC ${TGFX_FILES})",
                      "add_library(tgfx SHARED ${TGFX_FILES})",
                      "add_library(tgfx SHARED ${TGFX_FILES})");
                Patch(source, "merge_libraries_into(tgfx ${TGFX_STATIC_LIBS})",
                      "if (ANDROID)\n"
                      + "    target_link_libraries(tgfx ${TGFX_STATIC_LIBS})\n"
                      + "else ()\n"
                      + "merge_libraries_into(tgfx ${TGFX_STATIC_LIBS})\n"
                      + "endif ()",
                      "target_link_libraries(tgfx ${TGFX_STATIC_LIBS})");
                Patch(source, "set_target_properties(tgfx PROPERTIES PREFIX \"\")",
                      "if (ANDROID)\n"
                      + "    set_target_properties(tgfx PROPERTIES PREFIX \"lib\")\n"
                      + "else ()\n"
                      + "set_target_properties(tgfx PROPERTIES PREFIX \"\")\n"
                      + "endif ()",
                      "set_target_properties(tgfx PROPERTIES PREFIX \"lib\")");
                Patch(source, "set(CMAKE_CXX_VISIBILITY_PRESET hidden)",
                      "if (NOT ANDROID)\n"
                      + "set(CMAKE_CXX_VISIBILITY_PRESET hidden)\n"
                      + "endif ()",
                      "if (NOT ANDROID)\nset(CMAKE_CXX_VISIBILITY_PRESET hidden)");
            }

            string build;
            string library;
            string configure = string.Empty;
            if (android)
            {
                ndk ??= Environment.GetEnvironmentVariable("ANDROID_NDK_HOME");
                ndk ??= Environment.GetEnvironmentVariable("ANDROID_NDK_ROOT");
                if (string.IsNullOrEmpty(ndk))
                {
                    throw new BuilderException(
                        "tgfx needs the NDK path: pass --ndk=<dir> or set ANDROID_NDK_HOME");
                }
                build = context.Path("build", "tgfx", "android", mode);
                library = Path.Combine(build, "libtgfx.so");
                configure =
                    $"-DCMAKE_TOOLCHAIN_FILE=\"{Path.Combine(ndk, "build", "cmake", "android.toolchain.cmake")}\" "
                    + $"-DANDROID_NDK=\"{ndk}\" -DANDROID_ABI={arch} "
                    + $"-DANDROID_PLATFORM=android-{sdkVersion} -DANDROID_STL=c++_shared "
                    + "-DCMAKE_C_VISIBILITY_PRESET=default -DCMAKE_CXX_VISIBILITY_PRESET=default";
            }
            else
            {
                build = context.Path("build", "tgfx", mode);
                library = Path.Combine(build, "tgfx.lib");
            }

            List<string> lines = ["@echo off"];
            if (!android)
            {
                string? install = VisualStudioInstall();
                if (install is not null)
                {
                    string vcvars = Path.Combine(install, "VC", "Auxiliary", "Build",
                                                 "vcvars64.bat");
                    lines.Add($"call \"{vcvars}\" >nul || exit /b 1");
                }
            }
            lines.Add($"cmake -S \"{source}\" -B \"{build}\" -G Ninja "
                      + $"-DCMAKE_BUILD_TYPE={configuration} {configure} {Flags()} || exit /b 1");
            lines.Add($"cmake --build \"{build}\" --target tgfx || exit /b 1");

            Directory.CreateDirectory(build);
            string batch = Path.Combine(build, "build-tgfx.bat");
            File.WriteAllText(batch, string.Join("\r\n", lines), Encoding.ASCII);
            int status = ProcessRunner.Run("cmd", ["/c", batch]);
            if (status != 0)
            {
                throw new BuilderException($"tgfx build failed (exit {status})");
            }

            if (android && File.Exists(library))
            {
                string? strip = FindStrip(ndk);
                if (strip is not null)
                {
                    ProcessRunner.Run(strip, ["--strip-unneeded", library]);
                }
            }
            if (!File.Exists(library))
            {
                throw new BuilderException($"tgfx build did not produce {library}");
            }
            context.Output.WriteLine($"tgfx: {library}");
            return 0;
        }
        catch (BuilderException error)
        {
            context.Error.WriteLine($"tgfx: {error.Message}");
            return 1;
        }
    }

    private static string Flags()
    {
        return "-DTGFX_USE_VULKAN=ON -DTGFX_USE_OPENGL=OFF "
               + "-DTGFX_USE_FREETYPE=ON -DTGFX_BUILD_TESTS=OFF "
               + "-DTGFX_BUILD_SVG=OFF -DTGFX_BUILD_PDF=OFF "
               + "-DTGFX_BUILD_LAYERS=ON -DTGFX_BUILD_HELLO2D=OFF "
               + "-DTGFX_USE_JPEG_DECODE=OFF -DTGFX_USE_JPEG_ENCODE=OFF "
               + "-DTGFX_USE_WEBP_DECODE=OFF -DTGFX_USE_WEBP_ENCODE=OFF";
    }

    /* Rewrites one line of tgfx's CMakeLists in place, skipped once its marker
       is present so re-runs are no-ops. */
    private static void Patch(string source, string needle, string replacement, string marker)
    {
        string cmake = Path.Combine(source, "CMakeLists.txt");
        if (!File.Exists(cmake))
        {
            return;
        }
        string text = File.ReadAllText(cmake);
        if (text.Contains(marker, StringComparison.Ordinal))
        {
            return;
        }
        int at = text.IndexOf(needle, StringComparison.Ordinal);
        if (at < 0)
        {
            return;
        }
        File.WriteAllText(cmake, text[..at] + replacement + text[(at + needle.Length)..]);
    }

    private static string? FindStrip(string? ndk)
    {
        if (string.IsNullOrEmpty(ndk))
        {
            return null;
        }
        string prebuilt = Path.Combine(ndk, "toolchains", "llvm", "prebuilt");
        if (!Directory.Exists(prebuilt))
        {
            return null;
        }
        foreach (string host in Directory.GetDirectories(prebuilt))
        {
            string candidate = Path.Combine(host, "bin",
                                            OperatingSystem.IsWindows() ? "llvm-strip.exe"
                                                                        : "llvm-strip");
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }
        return null;
    }

    private static string? VisualStudioInstall()
    {
        string programFilesX86 = Environment.GetEnvironmentVariable("ProgramFiles(x86)")
                                 ?? @"C:\Program Files (x86)";
        string vswhere = Path.Combine(programFilesX86, "Microsoft Visual Studio", "Installer",
                                      "vswhere.exe");
        if (!File.Exists(vswhere))
        {
            return null;
        }
        (int status, string output) = ProcessRunner.Capture(vswhere,
        [
            "-latest", "-products", "*", "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath",
        ]);
        if (status != 0)
        {
            return null;
        }
        string install = output.Trim();
        return install.Length == 0 ? null : install;
    }
}
