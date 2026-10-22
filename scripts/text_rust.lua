-- Builds the text island's Rust static library. Run by packages/text/xmake.lua
-- through `xmake lua`, which gives this script the full xmake environment:
-- cargo needs the Visual Studio linker and LIB paths that only vcvars sets.

local project = os.projectdir()
local rust_root = path.join(project, "build", "text-rust")
local triple = is_plat("android") and "aarch64-linux-android" or "x86_64-pc-windows-msvc"

local program_files_x86 = os.getenv("ProgramFiles(x86)") or "C:\\Program Files (x86)"
local vswhere = path.join(program_files_x86, "Microsoft Visual Studio", "Installer", "vswhere.exe")
local lines = {"@echo off"}
if os.isfile(vswhere) then
    local install = os.iorunv(vswhere, {"-latest", "-products", "*", "-requires",
                                        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                        "-property", "installationPath"})
    install = install:gsub("%s+$", "")
    if install ~= "" then
        local vcvars = path.join(install, "VC", "Auxiliary", "Build", "vcvars64.bat")
        table.insert(lines, string.format('call "%s" >nul || exit /b 1', vcvars))
    end
end
table.insert(lines, string.format('set "CARGO_TARGET_DIR=%s"', rust_root))
if is_plat("android") then
    local ndk = get_config("ndk") or os.getenv("ANDROID_NDK_HOME") or
                    os.getenv("ANDROID_NDK_ROOT")
    if ndk == nil or ndk == "" then
        raise("no NDK path: pass --ndk=<dir> or set ANDROID_NDK_HOME")
    end
    local bin = path.join(ndk, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin")
    table.insert(lines, string.format(
                     'set "CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=%s"',
                     path.join(bin, "aarch64-linux-android29-clang.cmd")))
    table.insert(lines, string.format('set "CARGO_TARGET_AARCH64_LINUX_ANDROID_AR=%s"',
                                      path.join(bin, "llvm-ar.exe")))
end
table.insert(lines, string.format(
                 'cargo build --manifest-path "%s" --release --target %s',
                 path.join(project, "packages", "text", "rust", "Cargo.toml"), triple))

os.mkdir(rust_root)
local batch = path.join(rust_root, "build-rust.bat")
io.writefile(batch, table.concat(lines, "\r\n"))
os.execv("cmd", {"/c", batch})
