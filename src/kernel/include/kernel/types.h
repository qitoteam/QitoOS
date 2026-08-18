/*
 * Qira OS - fundamental kernel types
 */
#ifndef QIRA_TYPES_H
#define QIRA_TYPES_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed int         int32_t;
typedef unsigned int       uint32_t;
typedef signed long long   int64_t;
typedef unsigned long long uint64_t;

typedef uint64_t size_t;
typedef int64_t  ssize_t;
typedef uint64_t uintptr_t;
typedef int64_t  intptr_t;
typedef int64_t  off_t;

typedef uint64_t phys_addr_t;
typedef uint64_t virt_addr_t;

typedef int      bool_t;

#define NULL  ((void *)0)
#define true  1
#define false 0

#ifndef __cplusplus
typedef _Bool bool;
#endif

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255U
#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535U
#define INT32_MAX  2147483647
#define INT32_MIN  (-2147483647 - 1)
#define UINT32_MAX 4294967295U
#define INT64_MAX  9223372036854775807LL
#define INT64_MIN  (-9223372036854775807LL - 1)
#define UINT64_MAX 18446744073709551615ULL
#define SIZE_MAX   UINT64_MAX

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(d, s)      __builtin_va_copy(d, s)

/* Common helpers */
#define ARRAY_SIZE(a)      (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)          ((a) < (b) ? (a) : (b))
#define MAX(a, b)          ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi)   MIN(MAX((v), (lo)), (hi))
#define ALIGN_UP(v, a)     (((v) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(v, a)   ((v) & ~((a) - 1))
#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))

#define PACKED     __attribute__((packed))
#define ALIGNED(x) __attribute__((aligned(x)))
#define NORETURN   __attribute__((noreturn))
#define UNUSED(x)  ((void)(x))
#define INLINE     static inline __attribute__((always_inline))
#define PRINTF_FMT(f, a) __attribute__((format(printf, f, a)))

#define containerof(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

#endif /* QIRA_TYPES_H */
