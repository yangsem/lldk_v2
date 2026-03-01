#ifndef __LLDK_COMMON_H__
#define __LLDK_COMMON_H__

// os type
#if defined(_WIN32) || defined(_WIN64)
#define LLDK_OS_WINDOWS
#elif defined(__linux__)
#define LLDK_OS_LINUX
#elif defined(__APPLE__)
#define LLDK_OS_APPLE
#else
#error "Unsupported OS"
#endif

// architecture type
#if defined(__x86_64__) || defined(__amd64__)
#define LLDK_ARCH_X86_64
#elif defined(__i386__) || defined(__i486__) || defined(__i586__) || defined(__i686__)
#define LLDK_ARCH_X86
#elif defined(__arm__) || defined(__aarch64__)
#define LLDK_ARCH_ARM
#else
#error "Unsupported architecture"
#endif

// c++ standard
#if defined(__cplusplus) && __cplusplus >= 201103L
#define LLDK_CPP11
#elif defined(__cplusplus) && __cplusplus >= 201402L
#define LLDK_CPP14
#elif defined(__cplusplus) && __cplusplus >= 201703L
#define LLDK_CPP17
#elif defined(__cplusplus) && __cplusplus >= 202002L
#define LLDK_CPP20
#elif defined(__cplusplus) && __cplusplus >= 202303L
#define LLDK_CPP23
#else
#error "Unsupported C++ standard"
#endif

// export
#ifndef LLDK_OS_WINDOWS
#define LLDK_EXPORT __attribute__((visibility("default")))
#else
#define LLDK_EXPORT __declspec(dllexport)
#endif

// extern "C"
#ifdef __cplusplus
#define LLDK_EXTERN_C extern "C"
#else
#define LLDK_EXTERN_C
#endif

// endian type
#if defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define LLDK_ENDIAN_LITTLE
#elif defined(__BIG_ENDIAN__) || \
      (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define LLDK_ENDIAN_BIG
#else
#error "Unsupported endian"
#endif

// cacheline
#define LLDK_CACHELINE_SIZE 64
#define LLDK_CACHE_ALIGN(x) __attribute__((aligned(LLDK_CACHELINE_SIZE)))

// align
#define LLDK_ALIGN_BASE(n, base) (((n) + (base) - 1) & ~((base) - 1))
#define LLDK_ALIGN8(n) LLDK_ALIGN_BASE(n, 8)
#define LLDK_ALIGN16(n) LLDK_ALIGN_BASE(n, 16)
#define LLDK_ALIGN32(n) LLDK_ALIGN_BASE(n, 32)
#define LLDK_ALIGN64(n) LLDK_ALIGN_BASE(n, 64)
#define LLDK_ALIGN128(n) LLDK_ALIGN_BASE(n, 128)

// likely
#ifndef LLDK_OS_WINDOWS
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

// inline
#ifdef LLDK_OS_WINDOWS
#elif defined(LLDK_OS_LINUX) || defined(LLDK_OS_APPLE)
#define LLDK_INLINE __attribute__((always_inline)) inline
#else 
#define LLDK_INLINE inline
#endif

// access
#if defined (__cplusplus)
#define LLDK_ACCESS_ONCE(x) (*(volatile decltype(x)*)&(x))
#else
#ifndef LLDK_OS_WINDOWS
#define LLDK_ACCESS_ONCE(x) (*(volatile typeof(x)*)&(x))
#else
#define LLDK_ACCESS_ONCE(x) (x)
#endif
#endif

// unused
#define LLDK_UNUSED(x) (void)(x)

// max name length
#define LLDK_MAX_NAME_LENGTH 64
// max path length
#define LLDK_MAX_PATH_LENGTH 1024

// code position
#define __LLDK_TO_STRING(x) #x
#define __LLDK_TO_STRING2(x) __LLDK_TO_STRING(x)
#define LLDK_CODE_POSITION __FILE__ ":" __LLDK_TO_STRING2(__LINE__), __FUNCTION__
#define LLDK_CODE_POSITON_FORMAT "(%s,%s)"

// color
#ifdef LLDK_OS_LINUX
#define LLDK_COLOR_RED "\033[31m"
#define LLDK_COLOR_GREEN "\033[32m"
#define LLDK_COLOR_YELLOW "\033[33m"
#define LLDK_COLOR_BLUE "\033[34m"
#define LLDK_COLOR_MAGENTA "\033[35m"
#define LLDK_COLOR_CYAN "\033[36m"
#define LLDK_COLOR_RESET "\033[0m"
#else
#define LLDK_COLOR_RED ""
#define LLDK_COLOR_GREEN ""
#define LLDK_COLOR_YELLOW ""
#define LLDK_COLOR_BLUE ""
#define LLDK_COLOR_MAGENTA ""
#define LLDK_COLOR_CYAN ""
#define LLDK_COLOR_RESET ""
#endif

// print
#define LLDK_PRINT_BASE(channel, format, ...)                                  \
  {                                                                            \
    fprintf(channel, format LLDK_CODE_POSITON_FORMAT, ##__VA_ARGS__,           \
            LLDK_CODE_POSITION);                                               \
  }
#define LLDK_PRINT_INFO(format, ...) LLDK_PRINT_BASE(stdout, format, ##__VA_ARGS__)
#define LLDK_PRINT_WARN(format, ...) LLDK_PRINT_BASE(stdout, LLDK_COLOR_GREEN format LLDK_COLOR_RESET, ##__VA_ARGS__)
#define LLDK_PRINT_ERROR(format, ...) LLDK_PRINT_BASE(stderr, LLDK_COLOR_RED format LLDK_COLOR_RESET, ##__VA_ARGS__)

// common headers
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
// new
#ifdef __cplusplus
#include <new>
#define LLDK_NEW new(std::nothrow)
#endif

#endif // __LLDK_COMMON_H__

