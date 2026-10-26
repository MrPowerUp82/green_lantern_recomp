// Shim minimo de xenia/base/platform.h.
#pragma once

#if defined(_MSC_VER)
#define XE_FORCEINLINE __forceinline
#define XE_NOINLINE __declspec(noinline)
#define XE_NOALIAS __declspec(noalias)
#else
#define XE_FORCEINLINE inline __attribute__((always_inline))
#define XE_NOINLINE __attribute__((noinline))
#define XE_NOALIAS
#endif
#define XE_HOST_CACHE_LINE_SIZE 64
