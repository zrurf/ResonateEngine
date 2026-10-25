-- Fetches and builds tgfx with its Vulkan backend. tgfx and its vendored
-- dependencies are a build artifact: the checkout and the CMake build tree both
-- live under build/, so neither is committed. Run with `xmake tgfx`; a target
-- that links tgfx calls this first.
--
-- The build follows the current xmake configuration: Windows produces
-- build/tgfx/<mode>/tgfx.lib through the MSVC toolchain, Android produces
-- build/tgfx/android/<mode>/libtgfx.a through the NDK toolchain file (the
-- engine's c++_shared STL, so the archives must not carry their own).

import("core.project.config")
import("core.project.project")

config.load()

local project = os.projectdir()
local plat = config.get("plat") or os.host()
-- RESONATE_TGFX_MODE wins over the configuration: the Windows host keeps the
-- release archive even in debug builds (the C ABI boundary makes that safe).
local mode = os.getenv("RESONATE_TGFX_MODE") or config.get("mode") or "debug"
local source = path.join(project, "build", "third_party", "tgfx")
local cmake_type = mode == "release" and "Release" or "Debug"

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

-- On Android the archive is built shared: the ~800-object static archive
-- exceeds the Windows command line for both CMake's archive rule and the
-- vendor merge tool, while a shared library links through a response file and
-- is how tgfx ships on Android anyway. The merge POST_BUILD assumes a static
-- input (llvm-ar x on the target), so the vendor archives link into the .so
-- instead, and the empty PREFIX would name the library tgfx.so while the
-- Android linker resolves DT_NEEDED by the soname (libtgfx.so).
if plat == "android" then
    local cmake = path.join(source, "CMakeLists.txt")
    local text = io.readfile(cmake)

    -- Every edit works on the same in-memory copy; each is skipped once its
    -- marker is present, so re-runs are no-ops.
    local function wrap(needle, replacement, marker)
        if text:find(marker, 1, true) == nil then
            local first, last = text:find(needle, 1, true)
            if first then
                text = text:sub(1, first - 1) .. replacement .. text:sub(last + 1)
            end
        end
    end

    wrap("add_library(tgfx STATIC ${TGFX_FILES})",
         "add_library(tgfx SHARED ${TGFX_FILES})",
         "add_library(tgfx SHARED ${TGFX_FILES})")
    wrap("merge_libraries_into(tgfx ${TGFX_STATIC_LIBS})",
         "if (ANDROID)\n"
             .. "    target_link_libraries(tgfx ${TGFX_STATIC_LIBS})\n"
             .. "else ()\n"
             .. "merge_libraries_into(tgfx ${TGFX_STATIC_LIBS})\n"
             .. "endif ()",
         "target_link_libraries(tgfx ${TGFX_STATIC_LIBS})")
    -- Later set_target_properties calls win, so this rewrites the original
    -- line instead of adding an earlier one.
    wrap('set_target_properties(tgfx PROPERTIES PREFIX "")',
         "if (ANDROID)\n"
             .. "    set_target_properties(tgfx PROPERTIES PREFIX \"lib\")\n"
             .. "else ()\n"
             .. "set_target_properties(tgfx PROPERTIES PREFIX \"\")\n"
             .. "endif ()",
         'set_target_properties(tgfx PROPERTIES PREFIX "lib")')
    -- tgfx forces hidden visibility globally; the wrapper resolves tgfx's
    -- C++ API across the shared boundary, so Android keeps the default.
    wrap("set(CMAKE_CXX_VISIBILITY_PRESET hidden)",
         "if (NOT ANDROID)\n"
             .. "set(CMAKE_CXX_VISIBILITY_PRESET hidden)\n"
             .. "endif ()",
         "if (NOT ANDROID)\nset(CMAKE_CXX_VISIBILITY_PRESET hidden)")

    io.writefile(cmake, text)
end

local function tgfx_flags()
    return "-DTGFX_USE_VULKAN=ON -DTGFX_USE_OPENGL=OFF "
           .. "-DTGFX_USE_FREETYPE=ON -DTGFX_BUILD_TESTS=OFF "
           .. "-DTGFX_BUILD_SVG=OFF -DTGFX_BUILD_PDF=OFF "
           .. "-DTGFX_BUILD_LAYERS=ON -DTGFX_BUILD_HELLO2D=OFF "
           .. "-DTGFX_USE_JPEG_DECODE=OFF -DTGFX_USE_JPEG_ENCODE=OFF "
           .. "-DTGFX_USE_WEBP_DECODE=OFF -DTGFX_USE_WEBP_ENCODE=OFF"
end

local build = nil
local library = nil
local configure = nil

if plat == "android" then
    local ndk = config.get("ndk")
    if ndk == nil or ndk == "" then
        ndk = os.getenv("ANDROID_NDK_HOME") or os.getenv("ANDROID_NDK_ROOT")
    end
    if ndk == nil or ndk == "" then
        raise("tgfx needs the NDK path: pass --ndk=<dir> or set ANDROID_NDK_HOME")
    end
    local arch = config.get("arch") or "arm64-v8a"
    local sdkver = config.get("ndk_sdkver") or 30
    build = path.join(project, "build", "tgfx", "android", mode)
    library = path.join(build, "libtgfx.so")
    configure = string.format(
        '-DCMAKE_TOOLCHAIN_FILE="%s" -DANDROID_NDK="%s" -DANDROID_ABI=%s '
            .. '-DANDROID_PLATFORM=android-%d -DANDROID_STL=c++_shared '
            -- The NDK hides symbols by default; the wrapper resolves tgfx's
            -- C++ API across the shared boundary, so the .so must export it.
            .. '-DCMAKE_C_VISIBILITY_PRESET=default -DCMAKE_CXX_VISIBILITY_PRESET=default',
        path.join(ndk, "build", "cmake", "android.toolchain.cmake"), ndk, arch, sdkver)
else
    build = path.join(project, "build", "tgfx", mode)
    library = path.join(build, "tgfx.lib")
    configure = ""
end

local lines = {"@echo off"}
if plat ~= "android" then
    -- The MSVC toolchain comes from the environment vcvars64 sets; Ninja and
    -- cmake live on the PATH either way.
    local program_files_x86 = os.getenv("ProgramFiles(x86)") or "C:\\Program Files (x86)"
    local vswhere = path.join(program_files_x86, "Microsoft Visual Studio", "Installer",
                              "vswhere.exe")
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
end
table.insert(lines, string.format('cmake -S "%s" -B "%s" -G Ninja -DCMAKE_BUILD_TYPE=%s %s %s '
                                  .. "|| exit /b 1", source, build, cmake_type, configure,
                                  tgfx_flags()))
table.insert(lines, string.format('cmake --build "%s" --target tgfx || exit /b 1', build))

os.mkdir(build)
local batch = path.join(build, "build-tgfx.bat")
io.writefile(batch, table.concat(lines, "\r\n"))
os.execv("cmd", {"/c", batch})

if plat == "android" then
    -- The NDK keeps debug info even in Release, so the shipped library is
    -- stripped in place.
    if os.isfile(library) then
        local ndk = config.get("ndk") or os.getenv("ANDROID_NDK_HOME")
            or os.getenv("ANDROID_NDK_ROOT")
        local strip = path.join(ndk, "toolchains", "llvm", "prebuilt", "windows-x86_64",
                                "bin", "llvm-strip.exe")
        os.execv(strip, {"--strip-unneeded", library})
    end
end

if not os.isfile(library) then
    raise("tgfx build did not produce " .. library)
end
print("tgfx: " .. library)
