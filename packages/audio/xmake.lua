-- The audio domain's interface: what an output device exposes, so a plugin can
-- implement it and a consumer can reach it without linking an implementation. The
-- default implementation is packages/modules/audio.

target("Resonate.Audio.Abi")
    set_kind("headeronly")
    add_includedirs("include", {public = true})

    add_deps("Resonate.Module.Abi")
