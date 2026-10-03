-- Cross-checks the four places a module's contract is written down:
-- resonate.module.toml, src/module_descriptor.cpp, the capability headers, and
-- packages/modules/xmake.lua. Any disagreement fails the build. Declaring one
-- dependency set and linking another compiles cleanly and only shows up at
-- startup, which is expensive to diagnose.
--
-- Run with `xmake module-check`, or `xmake lua scripts/module_check.lua`.

local PROJECT_DIR = os.projectdir()
local MODULES_DIR = path.join(PROJECT_DIR, "packages", "modules")
local MODULES_BUILD = path.join(MODULES_DIR, "xmake.lua")
local HOST_MANIFEST = path.join(PROJECT_DIR, "resonate.host.toml")

-- Every field a module manifest may carry, which is also the set the schema
-- requires.
local MODULE_FIELDS = {"id", "name", "version", "abi", "min_host_abi", "summary", "provides", "requires",
                       "optional", "depends_on"}

local errors = {}

local function problem(message)
    errors[#errors + 1] = message
end

-- Manifests are TOML, but the checker takes a deliberately narrow subset: one
-- table header, [resonate.<kind>], with flat `key = value` lines inside whose
-- value is a string, integer, boolean, or a possibly multiline array of those.
-- Anything else TOML can express — other or nested tables, quoted headers,
-- dotted keys, fields outside the table — is reported as an error rather than
-- parsed, because the checker is what keeps manifests inside the subset every
-- reader (loader, editor, tooling) implements. A new construct arriving here is
-- a design change to the manifest format and has to land in this parser on
-- purpose.
local function readToml(file_path)
    local where = path.relative(file_path, PROJECT_DIR)
    local text = io.readfile(file_path)
    if text == nil then
        problem(string.format("cannot read %s", where))
        return nil
    end

    local sections = {}
    local current = nil
    local key = nil
    local key_chars = {}
    local value_chars = {}
    local header_chars = {}
    local mode = "key"
    local bracket = 0
    local in_string = false
    local in_header = false
    local header_closed = false
    local literal = false
    local escaped = false
    local line = 1

    local function scalar(raw)
        local quote = raw:sub(1, 1)
        if quote == '"' or quote == "'" then
            if #raw < 2 or raw:sub(-1) ~= quote then
                problem(string.format("%s:%d: '%s' has an unterminated string", where, line, key))
                return nil
            end
            local inner = raw:sub(2, -2)
            if quote == "'" then
                return inner
            end
            local escapes = {t = "\t", n = "\n", r = "\r", ['"'] = '"', ["\\"] = "\\"}
            return (inner:gsub("\\(.)", function(char)
                if escapes[char] == nil then
                    problem(string.format("%s:%d: unsupported escape \\%s in '%s'", where, line,
                                          char, key))
                    return ""
                end
                return escapes[char]
            end))
        end
        if raw == "true" then
            return true
        end
        if raw == "false" then
            return false
        end
        if raw:match("^%-?%d+$") then
            return tonumber(raw)
        end
        problem(string.format("%s:%d: unsupported value '%s' for '%s'", where, line, raw, key))
        return nil
    end

    local function flush()
        if key == nil then
            return
        end
        local raw = table.concat(value_chars):match("^%s*(.-)%s*$")
        if current == nil then
            problem(string.format("%s:%d: '%s' is outside any table; manifest fields live inside "
                                      .. "[resonate.<kind>]", where, line, key))
        elseif raw == "" then
            problem(string.format("%s:%d: '%s' has no value", where, line, key))
        elseif sections[current][key] ~= nil then
            problem(string.format("%s:%d: '%s' is defined twice", where, line, key))
        elseif raw:sub(1, 1) == "[" then
            if raw:sub(-1) ~= "]" then
                problem(string.format("%s:%d: '%s' has an unterminated array", where, line, key))
            else
                -- Split on commas outside string literals; nested arrays are left
                -- to scalar() to reject as unsupported values.
                local items = {}
                local item_chars = {}
                local quote = nil
                for index = 2, #raw - 1 do
                    local char = raw:sub(index, index)
                    if quote ~= nil then
                        item_chars[#item_chars + 1] = char
                        if char == quote then
                            quote = nil
                        end
                    elseif char == '"' or char == "'" then
                        quote = char
                        item_chars[#item_chars + 1] = char
                    elseif char == "," then
                        items[#items + 1] = table.concat(item_chars)
                        item_chars = {}
                    else
                        item_chars[#item_chars + 1] = char
                    end
                end
                items[#items + 1] = table.concat(item_chars)

                local array = {}
                for _, item in ipairs(items) do
                    local entry = item:match("^%s*(.-)%s*$")
                    if entry ~= "" then
                        array[#array + 1] = scalar(entry)
                    end
                end
                sections[current][key] = array
            end
        else
            sections[current][key] = scalar(raw)
        end
        key = nil
        value_chars = {}
    end

    local index = 1
    while index <= #text do
        local char = text:sub(index, index)
        if char == "\n" then
            if in_string then
                problem(string.format("%s:%d: unterminated string", where, line))
                in_string = false
            elseif in_header then
                problem(string.format("%s:%d: unterminated table header", where, line))
                in_header = false
                header_chars = {}
            elseif mode == "value" then
                if bracket > 0 then
                    -- A multiline array continues on the next line.
                    value_chars[#value_chars + 1] = " "
                else
                    flush()
                    mode = "key"
                end
            else
                local candidate = table.concat(key_chars):match("^%s*(.-)%s*$")
                if candidate ~= "" then
                    problem(string.format("%s:%d: '%s' has no '='", where, line, candidate))
                end
                key_chars = {}
            end
            header_closed = false
            line = line + 1
            index = index + 1
        elseif char == "\r" then
            index = index + 1
        elseif in_string then
            if escaped then
                value_chars[#value_chars + 1] = "\\"
                value_chars[#value_chars + 1] = char
                escaped = false
            elseif char == "\\" and not literal then
                escaped = true
            elseif char == (literal and "'" or '"') then
                in_string = false
                value_chars[#value_chars + 1] = char
            else
                value_chars[#value_chars + 1] = char
            end
            index = index + 1
        elseif in_header then
            if char == "]" then
                local name = table.concat(header_chars):match("^%s*(.-)%s*$")
                local namespace, kind = name:match("^([%w%-_]+)%.([%w%-_]+)$")
                if namespace ~= "resonate" or kind == nil then
                    problem(string.format("%s:%d: table '%s' is not engine TOML; manifest headers "
                                              .. "are [resonate.<kind>]", where, line, name))
                elseif sections[name] ~= nil then
                    problem(string.format("%s:%d: table [%s] is defined twice", where, line, name))
                else
                    sections[name] = {}
                    current = name
                end
                in_header = false
                header_closed = true
                header_chars = {}
            else
                header_chars[#header_chars + 1] = char
            end
            index = index + 1
        elseif char == "#" then
            while index <= #text and text:sub(index, index) ~= "\n" do
                index = index + 1
            end
        elseif mode == "value" and (char == '"' or char == "'") then
            in_string = true
            literal = char == "'"
            value_chars[#value_chars + 1] = char
            index = index + 1
        elseif mode == "key" then
            if char == "[" and not header_closed and
                table.concat(key_chars):match("^%s*$") ~= nil then
                in_header = true
                header_chars = {}
            elseif char == "=" then
                key = table.concat(key_chars):match("^%s*(.-)%s*$")
                if key:match("^[%w%-_]+$") == nil then
                    problem(string.format("%s:%d: invalid key '%s'", where, line, key))
                    key = nil
                end
                key_chars = {}
                value_chars = {}
                mode = "value"
            elseif header_closed and char ~= " " and char ~= "\t" then
                problem(string.format("%s:%d: unexpected text after a table header", where, line))
                header_closed = false
                key_chars[#key_chars + 1] = char
            else
                key_chars[#key_chars + 1] = char
            end
            index = index + 1
        else
            if char == "[" then
                bracket = bracket + 1
            elseif char == "]" then
                bracket = bracket - 1
            end
            value_chars[#value_chars + 1] = char
            index = index + 1
        end
    end

    flush()
    local leftover = table.concat(key_chars):match("^%s*(.-)%s*$")
    if leftover ~= "" then
        problem(string.format("%s:%d: '%s' has no '='", where, line, leftover))
    end
    if bracket ~= 0 then
        problem(string.format("%s: unbalanced '[' in '%s'", where, tostring(key)))
    end
    return sections
end

-- Manifests carry exactly one table, [resonate.<kind>], and every field lives
-- inside it; the table name is the document's kind marker, so a file whose
-- single table is a different kind is refused rather than half-read.
local function singleSection(sections, expected, label)
    local found = nil
    local count = 0
    for name in pairs(sections) do
        count = count + 1
        found = name
    end
    if count ~= 1 or found ~= expected then
        problem(string.format("%s: expected exactly one [%s] table", label, expected))
        return nil
    end
    return sections[found]
end

-- Shape is declared in schema/formats/module/*.schema.json for editors and CI
-- (taplo); this checker re-asserts required/unknown fields cheaply at build
-- time and enforces what a schema cannot see: the cross-file invariants below.
local function requireFields(manifest, label, fields)
    for _, field in ipairs(fields) do
        if manifest[field] == nil then
            problem(string.format("%s: manifest has no '%s'", label, field))
        end
    end
end

-- The other half of "additionalProperties: false" in the schema, so a field that
-- nothing reads cannot sit in a manifest unnoticed.
local function requireNoExtraFields(manifest, label, fields)
    local known = {}
    for _, field in ipairs(fields) do
        known[field] = true
    end
    for field in pairs(manifest) do
        if not known[field] then
            problem(string.format("%s: manifest has unknown field '%s'", label, field))
        end
    end
end

-- Index every capability declared in a header by name, so a manifest naming a
-- capability nobody defines is caught here rather than at resolve time.
--
-- The brace is optional in the type patterns because clang-format puts it on its
-- own line (Allman); a scanner that requires it on the type line silently finds
-- nothing the first time the tree is formatted.
--
-- A capability's name is the string its CapabilityTraits::name and ::id are both
-- built from, so that is what identifies it here.
local function scanDeclaredCapabilities()
    local by_name = {}
    local pattern = path.join(PROJECT_DIR, "packages", "**", "include", "**", "*.h")
    for _, header in ipairs(os.files(pattern)) do
        local current_type = nil
        local text = io.readfile(header)
        for line in text:gmatch("[^\r\n]+") do
            local type_name = line:match("^%s*typedef%s+struct%s+(%w+)%s*{?%s*$")
            if type_name == nil then
                type_name = line:match("^%s*struct%s+(%w+)%s*$")
            end
            if type_name ~= nil then
                current_type = type_name
            end

            local capability_name = line:match('const%s+char%s*%*%s*name%s*=%s*"([^"]*)"')
            if capability_name == nil then
                capability_name = line:match('Id%s+id%s*{"([^"]*)"}%s*;')
            end
            if capability_name ~= nil and current_type ~= nil then
                by_name[capability_name] = {type = current_type, header = header}
            end
        end
    end
    return by_name
end

-- The text between the parentheses of a macro call, found by balancing
-- parentheses rather than by assuming the call ends the file.
local function extractInvocation(text, macro_name)
    local start = text:find(macro_name .. "%s*%(")
    if start == nil then
        return nil
    end

    local open = text:find("(", start, true)
    local depth = 0
    local in_string = false
    local index = open

    while index <= #text do
        local char = text:sub(index, index)
        if in_string then
            if char == '"' then
                in_string = false
            end
        elseif char == '"' then
            in_string = true
        elseif char == "(" then
            depth = depth + 1
        elseif char == ")" then
            depth = depth - 1
            if depth == 0 then
                return text:sub(open + 1, index - 1)
            end
        end
        index = index + 1
    end
    return nil
end

-- Splits the arguments of a RESONATE_MODULE_DESCRIPTOR invocation. The arguments
-- are expressions rather than braced lists, so the split is on top-level commas
-- and has to step over <>, () and string literals.
local function splitTopLevel(invocation)
    local args = {}
    local current = {}
    local angle = 0
    local paren = 0
    local brace = 0
    local in_string = false
    local index = 1

    while index <= #invocation do
        local char = invocation:sub(index, index)
        if in_string then
            current[#current + 1] = char
            if char == '"' then
                in_string = false
            end
        elseif char == '"' then
            in_string = true
            current[#current + 1] = char
        elseif char == "<" then
            angle = angle + 1
            current[#current + 1] = char
        elseif char == ">" then
            angle = angle - 1
            current[#current + 1] = char
        elseif char == "(" then
            paren = paren + 1
            current[#current + 1] = char
        elseif char == ")" then
            paren = paren - 1
            current[#current + 1] = char
        elseif char == "{" then
            brace = brace + 1
            current[#current + 1] = char
        elseif char == "}" then
            brace = brace - 1
            current[#current + 1] = char
        elseif char == "," and angle == 0 and paren == 0 and brace == 0 then
            args[#args + 1] = table.concat(current)
            current = {}
        else
            current[#current + 1] = char
        end
        index = index + 1
    end

    args[#args + 1] = table.concat(current)
    return args
end

-- Argument order: id, display name, version, abi, then provides, requires,
-- optional, dependencies.
local function parseDescriptor(text)
    local invocation = extractInvocation(text, "RESONATE_MODULE_DESCRIPTOR")
    if invocation == nil then
        return nil
    end

    local args = splitTopLevel(invocation)

    local function stringArg(index)
        return args[index] and args[index]:match('^%s*"([^"]*)"') or nil
    end

    return {
        id = stringArg(1),
        name = stringArg(2),
        version = stringArg(3),
        abi = args[4] and tonumber(args[4]:match("%d+")) or nil,
        lists = {args[5], args[6], args[7], args[8]},
    }
end

-- Each list is written as RESONATE_CAPABILITIES(T...) or
-- RESONATE_MODULE_NAMES(...), so the types are either inside the angle brackets
-- or inside the parentheses.
local function parseTypeList(list)
    local types = {}
    if list == nil then
        return types
    end
    local arguments = list:match("<(.+)>") or list:match("%((.-)%)%s*$")
    if arguments == nil then
        return types
    end
    for type_name in arguments:gmatch("%w+") do
        types[#types + 1] = type_name
    end
    return types
end

local function parseNameList(list)
    local names = {}
    if list == nil then
        return names
    end
    for entry in list:gmatch('"([^"]*)"') do
        names[#names + 1] = entry
    end
    return names
end

-- Reads the module table out of packages/modules/xmake.lua. The table has to
-- stay one entry per line for this to find it; the format is documented there.
local function parseBuildTable()
    local text = io.readfile(MODULES_BUILD)
    if text == nil then
        problem("cannot read " .. path.relative(MODULES_BUILD, PROJECT_DIR))
        return {}
    end

    local entries = {}
    for line in text:gmatch("[^\r\n]+") do
        local dir, id, deps =
            line:match('{%s*dir%s*=%s*"(%w+)"%s*,%s*id%s*=%s*"([%w%.]+)"%s*,%s*deps%s*=%s*{(.-)}%s*}')
        if dir ~= nil then
            local names = {}
            for dependency in deps:gmatch('"([%w%.]+)"') do
                names[#names + 1] = dependency
            end
            entries[#entries + 1] = {dir = dir, id = id, deps = names}
        end
    end

    if #entries == 0 then
        problem("no module entries found in " .. path.relative(MODULES_BUILD, PROJECT_DIR))
    end
    return entries
end

local function sorted(list)
    local copy = {}
    for index, value in ipairs(list) do
        copy[index] = value
    end
    table.sort(copy)
    return table.concat(copy, ", ")
end

local function setDifference(a, b)
    local lookup = {}
    for _, value in ipairs(b) do
        lookup[value] = true
    end
    local result = {}
    for _, value in ipairs(a) do
        if not lookup[value] then
            result[#result + 1] = value
        end
    end
    return result
end

-- ---------------------------------------------------------------- validation

local declared = scanDeclaredCapabilities()
local host_sections = readToml(HOST_MANIFEST)
local host = host_sections ~= nil
                 and singleSection(host_sections, "resonate.host", "resonate.host.toml") or nil

if host ~= nil then
    requireFields(host, "resonate.host.toml", {"id", "name", "abi", "summary", "provides"})
    requireNoExtraFields(host, "resonate.host.toml", {"id", "name", "abi", "summary", "provides"})

    local manifests = {}
    local by_id = {}

    for _, manifest_path in ipairs(os.files(path.join(MODULES_DIR, "*", "resonate.module.toml"))) do
        local sections = readToml(manifest_path)
        if sections ~= nil then
            local dir = path.filename(path.directory(manifest_path))
            local manifest = singleSection(sections, "resonate.module", dir)
            if manifest ~= nil then
                requireFields(manifest, dir, MODULE_FIELDS)
                requireNoExtraFields(manifest, dir, MODULE_FIELDS)

                manifest.source = manifest_path
                manifest.dir = dir
                manifests[#manifests + 1] = manifest

                if manifest.id ~= nil then
                    if by_id[manifest.id] ~= nil then
                        problem(string.format("duplicate module id '%s'", manifest.id))
                    end
                    by_id[manifest.id] = manifest
                end
            end
        end
    end

    -- Everything a consumer may legitimately require.
    local available = {}
    for _, capability in ipairs(host.provides or {}) do
        available[capability] = "the host"
        if declared[capability] == nil then
            problem(string.format("host provides '%s' but no header declares it", capability))
        end
    end

    for _, manifest in ipairs(manifests) do
        local label = manifest.id or manifest.dir

        for _, capability in ipairs(manifest.provides or {}) do
            if available[capability] ~= nil then
                problem(string.format("%s provides '%s', already provided by %s", label, capability,
                                      available[capability]))
            end
            available[capability] = label
            if declared[capability] == nil then
                problem(string.format("%s provides '%s' but no header declares it", label, capability))
            end
        end

        for _, capability in ipairs(manifest.requires or {}) do
            if available[capability] == nil then
                problem(string.format("%s requires '%s', which nothing provides", label, capability))
            end
            if declared[capability] == nil then
                problem(string.format("%s requires '%s', which no header declares", label, capability))
            end
        end

        for _, capability in ipairs(manifest.optional or {}) do
            if declared[capability] == nil then
                problem(string.format("%s lists '%s' as optional but no header declares it", label, capability))
            end
        end

        for _, dependency in ipairs(manifest.depends_on or {}) do
            if by_id[dependency] == nil then
                problem(string.format("%s depends on '%s', which is not a known module", label, dependency))
            end
        end
    end

    -- Manifest versus build table. Both directions matter: a build entry with no
    -- manifest is a module nobody described, and a manifest with no entry is a
    -- module nobody builds, which would otherwise pass as consistent while never
    -- appearing in the plugin directory.
    local built = {}
    for _, entry in ipairs(parseBuildTable()) do
        built[entry.id] = entry
        local manifest = by_id[entry.id]
        if manifest == nil then
            problem(string.format("build declares module '%s' but no manifest defines it", entry.id))
        elseif manifest.dir ~= entry.dir then
            problem(string.format("'%s' is built from '%s' but its manifest is in '%s'", entry.id, entry.dir,
                                  manifest.dir))
        end
    end

    for _, manifest in ipairs(manifests) do
        if built[manifest.id] == nil then
            problem(string.format("%s has a manifest but no entry in %s, so nothing builds it",
                                  manifest.id or manifest.dir,
                                  path.relative(MODULES_BUILD, PROJECT_DIR)))
        end
    end

    -- The descriptor generates min_host_abi from the module's own abi, so a
    -- manifest that says otherwise describes a module the C++ side cannot produce.
    for _, manifest in ipairs(manifests) do
        if manifest.min_host_abi ~= manifest.abi then
            problem(string.format("%s: min_host_abi %s but abi %s, and the descriptor derives one from the other",
                                  manifest.id or manifest.dir, tostring(manifest.min_host_abi),
                                  tostring(manifest.abi)))
        end
    end

    for _, manifest in ipairs(manifests) do
        local entry = built[manifest.id]
        if entry ~= nil and sorted(entry.deps) ~= sorted(manifest.depends_on or {}) then
            problem(string.format("%s: build links [%s] but manifest depends_on [%s]", manifest.id,
                                  sorted(entry.deps), sorted(manifest.depends_on or {})))
        end
    end

    -- Manifest versus descriptor and capability headers.
    for _, manifest in ipairs(manifests) do
        local label = manifest.id or manifest.dir
        local descriptor_path = path.join(path.directory(manifest.source), "src", "module_descriptor.cpp")
        local text = io.readfile(descriptor_path)

        if text == nil then
            problem(string.format("%s has no src/module_descriptor.cpp", label))
        else
            local descriptor = parseDescriptor(text)
            if descriptor == nil then
                problem(string.format("%s does not define RESONATE_MODULE_DESCRIPTOR", label))
            else
                if descriptor.id ~= manifest.id then
                    problem(string.format("%s: descriptor id '%s' but manifest says '%s'", label,
                                          tostring(descriptor.id), tostring(manifest.id)))
                end
                if descriptor.name ~= manifest.name then
                    problem(string.format("%s: descriptor display name '%s' but manifest says '%s'", label,
                                          tostring(descriptor.name), tostring(manifest.name)))
                end
                if descriptor.version ~= manifest.version then
                    problem(string.format("%s: descriptor version '%s' but manifest says '%s'", label,
                                          tostring(descriptor.version), tostring(manifest.version)))
                end
                if descriptor.abi ~= manifest.abi then
                    problem(string.format("%s: descriptor abi %s but manifest says %s", label,
                                          tostring(descriptor.abi), tostring(manifest.abi)))
                end

                for index, key in ipairs({"provides", "requires", "optional"}) do
                    local expected = {}
                    for _, capability in ipairs(manifest[key] or {}) do
                        local entry = declared[capability]
                        if entry ~= nil then
                            expected[#expected + 1] = entry.type
                        end
                    end

                    local actual = parseTypeList(descriptor.lists[index])
                    local missing = setDifference(expected, actual)
                    local extra = setDifference(actual, expected)

                    if #missing > 0 then
                        problem(string.format("%s: descriptor %s omits %s", label, key, sorted(missing)))
                    end
                    if #extra > 0 then
                        problem(string.format("%s: descriptor %s lists %s, which the manifest does not declare",
                                              label, key, sorted(extra)))
                    end
                end

                if sorted(parseNameList(descriptor.lists[4])) ~= sorted(manifest.depends_on or {}) then
                    problem(string.format("%s: descriptor dependencies [%s] but manifest says [%s]", label,
                                          sorted(parseNameList(descriptor.lists[4])),
                                          sorted(manifest.depends_on or {})))
                end
            end
        end
    end

    if #errors > 0 then
        for _, message in ipairs(errors) do
            print("  %s", message)
        end
        raise("module check failed with %d problem(s)", #errors)
    end

    local provider_count = 0
    for _ in pairs(available) do
        provider_count = provider_count + 1
    end
    print("module check: %d module(s), %d capability provider(s) consistent", #manifests, provider_count)
end
