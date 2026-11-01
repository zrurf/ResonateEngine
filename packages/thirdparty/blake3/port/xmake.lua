-- The portable C implementation on every target: one code path, no ISA
-- dispatch, so a cook on one machine hashes the same bytes as a runtime on
-- another.

add_rules("mode.release", "mode.debug")

target("blake3")
    set_kind("$(kind)")
    add_files("c/blake3.c", "c/blake3_dispatch.c", "c/blake3_portable.c")
    add_headerfiles("c/blake3.h")
    add_defines("BLAKE3_NO_SSE2", "BLAKE3_NO_SSE41", "BLAKE3_NO_AVX2", "BLAKE3_NO_AVX512")
