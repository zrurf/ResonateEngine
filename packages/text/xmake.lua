-- The text domain: HarfBuzz shaping over a Rust/Skrifa/zeno rasterizer. The
-- Rust static library is built by cargo into build/text-rust; only the C ABI
-- header crosses into C++.

local rust_root = path.join(os.projectdir(), "build", "text-rust")
local rust_triple = is_plat("android") and "aarch64-linux-android" or "x86_64-pc-windows-msvc"
local rust_libdir = path.join(rust_root, rust_triple, "release")

target("Resonate.Text")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")
    add_includedirs("rust/include", {private = true})

    add_packages("harfbuzz")

    if is_plat("windows") then
        -- The Rust standard library's stdio calls the native API directly.
        add_syslinks("ntdll")
    end

    add_linkdirs(rust_libdir, {private = true})
    add_links("resonate_text_rust", {private = true})

    -- The Rust library is built by scripts/text_rust.lua through `xmake lua`,
    -- which has the full environment the package script scope lacks.
    before_build(function (target)
        local script = path.join(os.projectdir(), "scripts", "text_rust.lua")
        local output = os.iorunv(os.programfile(), {"lua", script})
        if output ~= nil then
            print((output:gsub("%\27%[[%d;]*m", ""):gsub("%s*%{%s*%}%s*$", ""):gsub("\n+$", "")))
        end
    end)
