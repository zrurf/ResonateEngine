#ifndef RESONATE_PAL_PLATFORM_H
#define RESONATE_PAL_PLATFORM_H

/*
 * Platform, architecture and compiler traits.
 *
 * The build system supplies exactly one RESONATE_PLATFORM_* define.
 * RESONATE_PLATFORM_SIMULATOR is orthogonal to it: a simulator build is still
 * built for a device platform.
 */

#if defined(RESONATE_PLATFORM_WINDOWS)
#    define RESONATE_PLATFORM_NAME "windows"
#elif defined(RESONATE_PLATFORM_LINUX)
#    define RESONATE_PLATFORM_NAME "linux"
#elif defined(RESONATE_PLATFORM_MACOS)
#    define RESONATE_PLATFORM_NAME "macos"
#elif defined(RESONATE_PLATFORM_IOS)
#    define RESONATE_PLATFORM_NAME "ios"
#elif defined(RESONATE_PLATFORM_ANDROID)
#    define RESONATE_PLATFORM_NAME "android"
#elif defined(RESONATE_PLATFORM_HARMONYOS)
#    define RESONATE_PLATFORM_NAME "harmonyos"
#else
#    error "No RESONATE_PLATFORM_* define was supplied by the build system"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#    define RESONATE_ARCH_NAME "x86_64"
#    define RESONATE_ARCH_X86_64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#    define RESONATE_ARCH_NAME "arm64"
#    define RESONATE_ARCH_ARM64 1
#elif defined(__riscv) && __riscv_xlen == 64
#    define RESONATE_ARCH_NAME "riscv64"
#    define RESONATE_ARCH_RISCV64 1
#else
#    error "Unsupported architecture"
#endif

#if defined(__clang__)
#    define RESONATE_COMPILER_CLANG 1
#elif defined(_MSC_VER)
#    define RESONATE_COMPILER_MSVC 1
#elif defined(__GNUC__)
#    define RESONATE_COMPILER_GCC 1
#else
#    error "Unsupported compiler"
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#    define RESONATE_ENDIAN_BIG 1
#else
#    define RESONATE_ENDIAN_LITTLE 1
#endif

/* Cache-line size, for padding apart values that concurrent threads write. */
#define RESONATE_CACHE_LINE_SIZE 64

#if defined(__GNUC__) || defined(__clang__)
#    define RESONATE_FORCE_INLINE static inline __attribute__((always_inline))
#    define RESONATE_RESTRICT __restrict__
#    define RESONATE_LIKELY(x) __builtin_expect(!!(x), 1)
#    define RESONATE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#    define RESONATE_FORCE_INLINE static __forceinline
#    define RESONATE_RESTRICT __restrict
#    define RESONATE_LIKELY(x) (x)
#    define RESONATE_UNLIKELY(x) (x)
#endif

#define RESONATE_UNUSED(x) ((void)(x))

#endif /* RESONATE_PAL_PLATFORM_H */
