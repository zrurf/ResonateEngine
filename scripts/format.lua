-- Runs clang-format over the source tree. Kept as an explicit task rather than
-- a build step so a reformatted tree is always a deliberate commit.
--
-- Run with `xmake format`, or `xmake lua scripts/format.lua`.

local candidates = {"clang-format", "clang-format-19", "clang-format-18", "clang-format-17"}

-- There is no PATH lookup in this sandbox, so a candidate is considered found
-- when it runs. --version exits immediately and prints to the console.
local formatter = os.getenv("RESONATE_CLANG_FORMAT")
if not formatter then
    for _, candidate in ipairs(candidates) do
        if os.execv(candidate, {"--version"}) == 0 then
            formatter = candidate
            break
        end
    end
end

if not formatter then
    print("clang-format not found; set RESONATE_CLANG_FORMAT to run this")
    return
end

local patterns = {
    path.join(os.projectdir(), "packages", "**", "*.h"),
    path.join(os.projectdir(), "packages", "**", "*.hpp"),
    path.join(os.projectdir(), "packages", "**", "*.cpp"),
    path.join(os.projectdir(), "tests", "**", "*.h"),
    path.join(os.projectdir(), "tests", "**", "*.hpp"),
    path.join(os.projectdir(), "tests", "**", "*.cpp"),
}

local count = 0
local failed = 0
for _, pattern in ipairs(patterns) do
    for _, file in ipairs(os.files(pattern)) do
        -- A formatter that refuses a file has to be reported: counting it as
        -- formatted would print success over a tree that was not formatted.
        if os.execv(formatter, {"-i", "--style=file", file}) == 0 then
            count = count + 1
        else
            failed = failed + 1
            print("clang-format failed on %s", file)
        end
    end
end

print("formatted %d file(s)", count)
if failed > 0 then
    raise("clang-format failed on %d file(s)", failed)
end
