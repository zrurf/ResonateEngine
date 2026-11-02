using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Resonate.Builder.Commands;

/* Builds the text island's Rust static library. cargo needs the Visual Studio
   linker and LIB paths that only vcvars sets, so the build runs through a
   batch file that calls vcvars first; the Android target uses the NDK's clang
   wrapper and linker instead. */
internal static class RustCommand
{
    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate rust: unknown argument '{args[0]}'");
            return 2;
        }
        bool android = Environment.GetEnvironmentVariable("RESONATE_TEXT_PLATFORM") == "android";
        string triple = android ? "aarch64-linux-android" : "x86_64-pc-windows-msvc";
        string rustRoot = context.BuildPath("text-rust");
        List<string> lines = ["@echo off"];

        if (!android)
        {
            string? install = VisualStudioInstall();
            if (install is not null)
            {
                string vcvars = Path.Combine(install, "VC", "Auxiliary", "Build", "vcvars64.bat");
                lines.Add($"call \"{vcvars}\" >nul || exit /b 1");
            }
        }
        lines.Add($"set \"CARGO_TARGET_DIR={rustRoot}\"");
        if (android)
        {
            string? ndk = Environment.GetEnvironmentVariable("RESONATE_TEXT_NDK");
            if (string.IsNullOrEmpty(ndk))
            {
                context.Error.WriteLine("no NDK path: pass --ndk=<dir> or set ANDROID_NDK_HOME");
                return 1;
            }
            /* The user's cargo config applies -C target-cpu=native to host
               builds; that is meaningless and crashes rustc for an Android
               target. */
            lines.Add("set \"RUSTFLAGS=-g\"");
            string bin = Path.Combine(ndk, "toolchains", "llvm", "prebuilt", "windows-x86_64",
                                      "bin");
            lines.Add("set \"CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="
                      + Path.Combine(bin, "aarch64-linux-android29-clang.cmd") + "\"");
            lines.Add("set \"CARGO_TARGET_AARCH64_LINUX_ANDROID_AR="
                      + Path.Combine(bin, "llvm-ar.exe") + "\"");
        }
        lines.Add($"cargo build --manifest-path \"{context.Path("packages", "text", "rust", "Cargo.toml")}\" "
                  + $"--release --target {triple}");

        Directory.CreateDirectory(rustRoot);
        string batch = Path.Combine(rustRoot, "build-rust.bat");
        File.WriteAllText(batch, string.Join("\r\n", lines), Encoding.ASCII);
        int status = ProcessRunner.Run("cmd", ["/c", batch]);
        if (status != 0)
        {
            context.Error.WriteLine($"rust: cargo build failed (exit {status})");
            return 1;
        }
        return 0;
    }

    /* The VC tools component is what vcvars needs; the newest installation
       that has it wins, the same selection the Lua script made. */
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
