-- Publishes the C# builder once per xmake process and forwards commands to it.
-- The builder owns the SDK steps (schema, contracts, cook, packaging, tool
-- tasks); xmake only defines the C++ build graph, and this module is the one
-- bridge. Each xmake script has its own globals, so the publish logic lives
-- here instead of in every target that needs a generated header.

local exe = nil

local function program()
    if exe ~= nil then
        return exe
    end

    local project = os.projectdir()
    local out = path.join(project, "build", "tools", "builder", "publish")
    local binary = path.join(out, os.host() == "windows" and "resonate.exe" or "resonate")
    local stale = not os.isfile(binary)
    if not stale then
        local built_at = os.mtime(binary)
        for _, tool in ipairs({"schemac", "csbind", "builder"}) do
            for _, file in ipairs(os.files(path.join(project, "packages", "tools", tool, "**"))) do
                if os.mtime(file) >= built_at then
                    stale = true
                    break
                end
            end
            if stale then
                break
            end
        end
    end
    if stale then
        local csproj = path.join(project, "packages", "tools", "builder", "cli",
                                 "Resonate.Builder.Cli.csproj")
        local publish = {"publish", csproj, "-c", "Release", "-o", out, "--nologo", "-v", "quiet"}
        local status = os.execv("dotnet", publish, {try = true})
        if status ~= 0 then
            -- Two targets can reach the builder at once; a retry finds the
            -- winner's output in place.
            os.sleep(1000)
            status = os.execv("dotnet", publish, {try = true})
        end
        if status ~= 0 and not os.isfile(binary) then
            raise("builder: publishing the C# builder failed (dotnet publish exited %s); the .NET "
                      .. "SDK is required", tostring(status))
        end
    end
    exe = binary
    return exe
end

-- Runs a command and fails the calling hook when it fails.
function run(args, envs)
    local status = os.execv(program(), args, {try = true, envs = envs or {}})
    if status ~= 0 then
        raise("builder: '%s' failed (exit %s)", tostring(args[1]), tostring(status))
    end
end

-- Runs a command and returns its exit code: a task that forwards a tool whose
-- non-zero exit is part of its contract asks for the code.
function code(args, envs)
    return os.execv(program(), args, {try = true, envs = envs or {}})
end

