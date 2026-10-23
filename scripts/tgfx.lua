-- Fetches and builds tgfx with its Vulkan backend. tgfx and its vendored
-- dependencies are a build artifact: the checkout and the CMake build tree both
-- live under build/, so neither is committed. Run with `xmake tgfx`; a target
-- that links tgfx calls this first.
--
-- Windows only for now: the Android NDK build needs its own CMake toolchain
-- file and is a separate step.

local project = os.projectdir()
local source = path.join(project, "build", "third_party", "tgfx")
local build = path.join(project, "build", "tgfx")
local library = path.join(build, "tgfx.lib")

if not os.isdir(path.join(source, "include", "tgfx")) then
    os.mkdir(path.dirname(source))
    os.setenv("GIT_LFS_SKIP_SMUDGE", "1")
    os.execv("git", {"clone", "--depth", "1", "https://github.com/Tencent/tgfx.git", source})
end

if not os.isfile(path.join(source, "third_party", "vendor_tools", "vendor.cmake")) then
    local ok = pcall(function ()
        os.execv("depsync", {})
    end)
    if not ok then
        raise("tgfx needs its vendored dependencies: npm install -g depsync, then xmake tgfx")
    end
end

-- tgfx passes a clang-only warning flag when compiling volk, which MSVC rejects.
-- The guard is upstream-safe and keeps the vendored tree usable on both.
do
    local cmake = path.join(source, "CMakeLists.txt")
    local text = io.readfile(cmake)
    local old = "    set_source_files_properties(${VOLK_FILES} PROPERTIES COMPILE_OPTIONS -Wno-misleading-indentation)"
    local first, last = text:find(old, 1, true)
    if first then
        local replacement = "    if (NOT MSVC)\n" .. old .. "\n    endif ()"
        io.writefile(cmake, text:sub(1, first - 1) .. replacement .. text:sub(last + 1))
    end
end

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
table.insert(lines, string.format('cmake -S "%s" -B "%s" -G Ninja -DCMAKE_BUILD_TYPE=Release '
                                 .. "-DTGFX_USE_VULKAN=ON -DTGFX_USE_OPENGL=OFF "
                                 .. "-DTGFX_USE_FREETYPE=ON -DTGFX_BUILD_TESTS=OFF "
                                 .. "-DTGFX_BUILD_SVG=OFF -DTGFX_BUILD_PDF=OFF "
                                 .. "-DTGFX_BUILD_LAYERS=ON -DTGFX_BUILD_HELLO2D=OFF "
                                 .. "-DTGFX_USE_JPEG_DECODE=OFF -DTGFX_USE_JPEG_ENCODE=OFF "
                                 .. "-DTGFX_USE_WEBP_DECODE=OFF -DTGFX_USE_WEBP_ENCODE=OFF "
                                 .. "|| exit /b 1", source, build))
table.insert(lines, string.format('cmake --build "%s" --target tgfx || exit /b 1', build))

os.mkdir(build)
local batch = path.join(build, "build-tgfx.bat")
io.writefile(batch, table.concat(lines, "\r\n"))
os.execv("cmd", {"/c", batch})

if not os.isfile(library) then
    raise("tgfx build did not produce " .. library)
end
print("tgfx: " .. library)
