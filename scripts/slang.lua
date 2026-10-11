-- Compiles the modules' Slang shaders to SPIR-V plus reflection JSON, writing
-- build/shaders. Runnable by hand with `xmake slang`; a build of a target that
-- consumes the outputs will run the same body through its own hook (none yet).
--
-- slangc is the Vulkan SDK's, the same source the build's Resonate.Vulkan
-- target uses. The entry points come from the [shader(...)] attributes in each
-- source, and slangc validates the SPIR-V it emits unless told not to, so a
-- green exit is the compile check.

local PROJECT_DIR = os.projectdir()
local MODULES_DIR = path.join(PROJECT_DIR, "packages", "modules")
local OUT_DIR = path.join(PROJECT_DIR, "build", "shaders")

local function find_slangc()
    local sdk = os.getenv("RESONATE_VULKAN_SDK") or os.getenv("VULKAN_SDK")
    if sdk == nil or sdk == "" then
        raise("slang: no Vulkan SDK in the environment (VULKAN_SDK), so no slangc")
    end
    local bin = path.join(sdk, "Bin", is_host("windows") and "slangc.exe" or "slangc")
    if not os.isfile(bin) then
        raise("slang: slangc not found at %s", bin)
    end
    return bin
end

-- '[shader("vertex")]' names the stage; the first function definition line
-- after it is the entry point. Line-based, because a single pattern across the
-- whole source cannot stop before the entry-point name greedily. A source
-- whose entries go unrecognized yields none and fails the run below, so
-- pattern drift is visible rather than a silent skip.
local function entryPointsOf(source)
    local entries = {}
    local stage = nil
    for line in source:gmatch("[^\r\n]+") do
        if stage == nil then
            stage = line:match('^%s*%[shader%s*%(%s*"(%w+)"%s*%)%s*%]')
        elseif not line:match("^%s*//") then
            -- No underscore placeholders: the xmake lua sandbox breaks
            -- destructuring that names a local `_`.
            local _, name = line:match("^%s*(.-)(%w+)%s*%(")
            if name ~= nil then
                entries[#entries + 1] = {stage = stage, name = name}
                stage = nil
            end
        end
    end
    return entries
end

local function upToDate(path, source_mtime)
    return os.isfile(path) and os.mtime(path) > source_mtime
end

local slangc = find_slangc()
os.mkdir(OUT_DIR)

local shaders = 0
local compiled = 0
local entries_total = 0
for _, source in ipairs(os.files(path.join(MODULES_DIR, "*", "shaders", "*.slang"))) do
    shaders = shaders + 1
    local name = path.basename(source)
    local source_mtime = os.mtime(source)

    local entries = entryPointsOf(io.readfile(source))
    if #entries == 0 then
        raise("slang: %s declares no [shader(...)] entry points", source)
    end

    for _, entry in ipairs(entries) do
        entries_total = entries_total + 1
        local spv = path.join(OUT_DIR, name .. "." .. entry.name .. ".spv")
        local reflection = path.join(OUT_DIR, name .. "." .. entry.name .. ".refl.json")
        if upToDate(spv, source_mtime) and upToDate(reflection, source_mtime) then
            goto continue
        end

        local code = os.execv(slangc, {source, "-entry", entry.name, "-target", "spirv", "-o", spv,
                                       "-reflection-json", reflection}, {try = true})
        if code ~= 0 then
            raise("slang: compiling %s (%s) failed (exit %s)", source, entry.name, tostring(code))
        end
        compiled = compiled + 1
        ::continue::
    end
end

print(string.format("slangc: %d shader(s), %d entry point(s), %d written -> %s", shaders,
                    entries_total, compiled, path.relative(OUT_DIR, PROJECT_DIR)))
