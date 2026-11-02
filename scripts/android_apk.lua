-- Packages the built Android host, its plugin libraries and their manifests
-- into a signed APK. Run it after the build and after `xmake cook`:
-- xmake android-apk.
--
-- The SDK's own tools rather than Gradle: the shell has no Java code, so the
-- manifest names NativeActivity and the native entry point does the rest.

import("core.project.config")
import("core.project.project")

config.load()

local function ndk_directory()
    local ndk = config.get("ndk")
    if ndk == nil or ndk == "" then
        ndk = os.getenv("ANDROID_NDK_HOME") or os.getenv("ANDROID_NDK_ROOT")
    end
    if ndk == nil or ndk == "" then
        raise("no NDK path: pass --ndk=<dir> or set ANDROID_NDK_HOME")
    end
    return ndk
end

local function newest_version(directory)
    local best = nil
    local best_key = -1
    for _, dir in ipairs(os.dirs(path.join(directory, "*"))) do
        local major, minor, patch = path.basename(dir):match("(%d+)%.(%d+)%.?(%d*)")
        if major ~= nil then
            local key = tonumber(major) * 1000000 + tonumber(minor) * 1000 + (tonumber(patch) or 0)
            if key > best_key then
                best = dir
                best_key = key
            end
        end
    end
    return best
end

local host = project.target("Resonate.Host.Android")
local module = project.target("Resonate.Module.resonate.audio")
if host == nil or module == nil then
    raise("configure for Android first: xmake f -p android ...")
end
if not os.isfile(path.join(host:targetdir(), host:filename())) then
    raise("build the host first: xmake build Resonate.Host.Android")
end

-- The module targets write into <build>/plugins, so one of them names both the
-- plugin directory and the build root.
local plugins = path.absolute(module:targetdir())
local build = path.directory(plugins)
local abi = host:arch()

local ndk = ndk_directory()
local sdk = os.getenv("ANDROID_HOME") or os.getenv("ANDROID_SDK_ROOT")
if sdk == nil or sdk == "" then
    sdk = path.directory(path.directory(ndk))
end

local build_tools = newest_version(path.join(sdk, "build-tools"))
if build_tools == nil then
    raise("no Android build-tools under " .. path.join(sdk, "build-tools"))
end
local tool = os.host() == "windows" and ".exe" or ""
local aapt2 = path.join(build_tools, "aapt2" .. tool)
local zipalign = path.join(build_tools, "zipalign" .. tool)
local apksigner = path.join(build_tools, "lib", "apksigner.jar")

local platform = nil
local platform_version = -1
for _, dir in ipairs(os.dirs(path.join(sdk, "platforms", "android-*"))) do
    local version = tonumber(path.basename(dir):match("android%-(%d+)"))
    if version ~= nil and version > platform_version then
        platform = dir
        platform_version = version
    end
end
if platform == nil then
    raise("no Android platform under " .. path.join(sdk, "platforms"))
end

local staging = path.join(build, "apk")
os.tryrm(staging)
os.mkdir(path.join(staging, "lib", abi))
os.mkdir(path.join(staging, "assets", "plugins"))

os.cp(path.join(host:targetdir(), host:filename()), path.join(staging, "lib", abi))
for _, library in ipairs(os.files(path.join(plugins, "*.so"))) do
    os.cp(library, path.join(staging, "lib", abi))
end

-- libtgfx.so lives in its own CMake build tree; the wrapper .so beside the
-- plugins names it as a dependency, so it travels in the same ABI directory.
-- tgfx builds against the NDK's shared C++ runtime (the C ABI keeps its STL
-- objects from crossing the boundary), so the runtime travels with it.
local tgfx_dir = path.join(os.projectdir(), "build", "tgfx", "android", "release")
local ndk = ndk_directory()
local cxx_shared = path.join(ndk, "toolchains", "llvm", "prebuilt", "windows-x86_64",
                              "sysroot", "usr", "lib", "aarch64-linux-android",
                              "libc++_shared.so")
if os.isfile(path.join(tgfx_dir, "libtgfx.so")) then
    os.cp(path.join(tgfx_dir, "libtgfx.so"), path.join(staging, "lib", abi))
    if os.isfile(cxx_shared) then
        os.cp(cxx_shared, path.join(staging, "lib", abi))
    end
end

-- The loader pairs a library with `<id>.toml`; the manifests travel as assets
-- because the APK's library directory is read-only.
for _, directory in ipairs(os.dirs(path.join(os.projectdir(), "packages", "modules", "*"))) do
    local manifest = path.join(directory, "resonate.module.toml")
    if os.isfile(manifest) then
        local id = io.readfile(manifest):match('id%s*=%s*"([^"]+)"')
        if id ~= nil then
            os.cp(manifest, path.join(staging, "assets", "plugins", id .. ".toml"))
        end
    end
end

-- The renderer loads SPIR-V by name at pipeline creation; the host extracts
-- these assets and points it at them.
local shader_source = path.join(os.projectdir(), "build", "shaders")
local shaders = os.files(path.join(shader_source, "*.spv"))
if #shaders == 0 then
    raise("no built shaders under " .. shader_source .. "; run `xmake slang` first")
end
os.mkdir(path.join(staging, "assets", "shaders"))
for _, shader in ipairs(shaders) do
    os.cp(shader, path.join(staging, "assets", "shaders"))
end

-- Assets ship cooked: the source documents are XML and the package carries the
-- binary codec's form, which is also what keeps the authored text out of the
-- APK. The host extracts this directory and points the startup at the
-- document and its string table; the fonts stay in one flat directory because
-- the asset manager lists directories without nesting.
-- The cooked tree is platform-independent (the codecs store plain fields), so
-- one host-side cook serves every target; the task builds it.
local cooked = path.join(os.projectdir(), "build", "cooked")
if not os.isdir(cooked) then
    raise("no cooked content under " .. cooked .. "; run `xmake cook` first")
end
local ui_source = path.join(cooked, "ui")
local ui_assets = os.files(path.join(ui_source, "*.rui"))
local ui_sheets = os.files(path.join(ui_source, "*.rcss"))
local ui_strings = os.files(path.join(ui_source, "*.rstrings"))
if #ui_assets > 0 or #ui_sheets > 0 or #ui_strings > 0 then
    os.mkdir(path.join(staging, "assets", "ui"))
    for _, document in ipairs(ui_assets) do
        os.cp(document, path.join(staging, "assets", "ui"))
    end
    for _, sheet in ipairs(ui_sheets) do
        os.cp(sheet, path.join(staging, "assets", "ui"))
    end
    for _, strings in ipairs(ui_strings) do
        os.cp(strings, path.join(staging, "assets", "ui"))
    end
end

local ui_fonts = os.files(path.join(ui_source, "fonts", "*.ttf"))
if #ui_fonts > 0 then
    os.mkdir(path.join(staging, "assets", "fonts"))
    for _, font in ipairs(ui_fonts) do
        os.cp(font, path.join(staging, "assets", "fonts"))
    end
end

local ui_images = os.files(path.join(ui_source, "images", "*.png"))
if #ui_images > 0 then
    os.mkdir(path.join(staging, "assets", "images"))
    for _, image in ipairs(ui_images) do
        os.cp(image, path.join(staging, "assets", "images"))
    end
end

-- The demo scene document, cooked like the UI ones.
local scene_documents = os.files(path.join(cooked, "scene", "*.rscene"))
if #scene_documents > 0 then
    os.mkdir(path.join(staging, "assets", "scene"))
    for _, document in ipairs(scene_documents) do
        os.cp(document, path.join(staging, "assets", "scene"))
    end
end

-- Clips are cooked like the documents; the player resolves them by id when the
-- host stages a products manifest beside them.
local clips = os.files(path.join(cooked, "animation", "*.rclip"))
if #clips > 0 then
    os.mkdir(path.join(staging, "assets", "animation"))
    for _, clip in ipairs(clips) do
        os.cp(clip, path.join(staging, "assets", "animation"))
    end
end

local base = path.join(staging, "base.apk")
local aligned = path.join(staging, "aligned.apk")
local output = path.join(build, "resonate.apk")

-- The manifest's minimum tracks the API level the native code was compiled
-- against; a lower one would install libraries that cannot load.
local sdk_version = tostring(config.get("ndk_sdkver") or "29")

local function run(program, argv)
    local code = os.execv(program, argv)
    if code ~= 0 then
        raise("android apk: " .. program .. " failed with code " .. tostring(code))
    end
end

-- The Java activity owns the IME input connection (Android delivers composed
-- text to a View, not to the native window). There is no Gradle here, so javac
-- compiles the sources against android.jar and d8 turns the classes into the
-- single classes.dex the package carries.
local java_source = path.join(os.projectdir(), "packages", "host", "android", "java")
local java_files = os.files(path.join(java_source, "**.java"))
local dex_directory = path.join(staging, "dex")
if #java_files > 0 then
    local classes = path.join(staging, "classes")
    os.mkdir(classes)
    os.mkdir(dex_directory)
    local javac_argv = {"-source", "8", "-target", "8", "-nowarn",
                        "-classpath", path.join(platform, "android.jar"), "-d", classes}
    for _, file in ipairs(java_files) do
        table.insert(javac_argv, file)
    end
    run("javac", javac_argv)
    local d8 = path.join(build_tools, "d8" .. (os.host() == "windows" and ".bat" or ""))
    local d8_argv = {"--lib", path.join(platform, "android.jar"),
                     "--min-api", sdk_version, "--output", dex_directory}
    for _, file in ipairs(os.files(path.join(classes, "**.class"))) do
        table.insert(d8_argv, file)
    end
    run(d8, d8_argv)
end

-- The theme in res/ carries the display-cutout setting; aapt2 needs it
-- compiled before the link.
local resources = path.join(staging, "resources.zip")
run(aapt2, {"compile", "--dir", path.join(os.projectdir(), "packages", "host", "android", "res"),
            "-o", resources})
run(aapt2, {"link", "-o", base,
            "--manifest", path.join(os.projectdir(), "packages", "host", "android",
                                    "AndroidManifest.xml"),
            "-I", path.join(platform, "android.jar"),
            "--min-sdk-version", sdk_version,
            "--target-sdk-version", tostring(platform_version),
            "--version-code", "1", "--version-name", "0.1", resources})
-- jar rather than aapt2 -A: aapt2 on Windows writes the staged assets with
-- backslashes in their entry names, which the asset manager looks up with
-- forward slashes.
run("jar", {"--update", "--file", base,
            "-C", staging, "lib", "-C", staging, "assets"})
if os.isfile(path.join(dex_directory, "classes.dex")) then
    run("jar", {"--update", "--file", base, "-C", dex_directory, "classes.dex"})
end
run(zipalign, {"-f", "-p", "4", base, aligned})

local keystore = path.join(build, "debug.keystore")
if not os.isfile(keystore) then
    run("keytool", {"-genkeypair", "-keystore", keystore,
                    "-storepass", "android", "-keypass", "android",
                    "-alias", "androiddebugkey", "-keyalg", "RSA", "-keysize", "2048",
                    "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"})
end
-- A small heap: apksigner needs little, and the default reservation fails on
-- machines with a tight commit limit.
run("java", {"-Xmx256m", "-Xss1m", "-jar", apksigner, "sign",
             "--ks", keystore, "--ks-key-alias", "androiddebugkey",
             "--ks-pass", "pass:android", "--key-pass", "pass:android",
             "--out", output, aligned})

print("android: " .. output)
