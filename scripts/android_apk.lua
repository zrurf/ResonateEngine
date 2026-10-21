-- Packages the built Android host, its plugin libraries and their manifests
-- into a signed APK. Run it after the build: xmake android-apk.
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
run(zipalign, {"-f", "-p", "4", base, aligned})

local keystore = path.join(build, "debug.keystore")
if not os.isfile(keystore) then
    run("keytool", {"-genkeypair", "-keystore", keystore,
                    "-storepass", "android", "-keypass", "android",
                    "-alias", "androiddebugkey", "-keyalg", "RSA", "-keysize", "2048",
                    "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"})
end
run("java", {"-jar", apksigner, "sign",
             "--ks", keystore, "--ks-key-alias", "androiddebugkey",
             "--ks-pass", "pass:android", "--key-pass", "pass:android",
             "--out", output, aligned})

print("android: " .. output)
