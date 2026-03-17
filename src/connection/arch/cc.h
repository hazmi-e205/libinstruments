/*
 * lwIP architecture configuration for libinstruments
 * Cross-platform: Windows (MinGW/MSVC), Linux, macOS
 *
 * This provides the platform-specific types and macros that lwIP needs
 * when running in NO_SYS (bare-metal) mode for userspace networking.
 */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

/* --- Platform detection --- */
#ifdef _WIN32

#ifdef _MSC_VER
#pragma warning(disable: 4127) /* conditional expression is constant */
#pragma warning(disable: 4996) /* deprecated functions */
#endif

#include <errno.h>

/* Endianness - Windows is always little-endian on supported platforms */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#else /* POSIX (Linux, macOS) */

#define LWIP_TIMEVAL_PRIVATE 0
#include <sys/time.h>
#include <errno.h>

#ifdef __APPLE__
#include <sys/types.h>
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS
#endif

#endif /* _WIN32 */

/* --- Common configuration --- */

/* Protection type for critical sections (unused in NO_SYS mode) */
typedef int sys_prot_t;

/* Compiler hints for packing structures */
#if defined(__GNUC__) || defined(__clang__)
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#elif defined(_MSC_VER)
#define PACK_STRUCT_USE_INCLUDES
#endif

/* Platform diagnostic output */
#include <stdio.h>

#if defined(__cplusplus)
#if defined(__has_include)
#if __has_include(<QDebug>)
#include <QDebug>
#define LWIP_PLATFORM_DIAG(x) do { qDebug().noquote().nospace() << x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    qCritical().noquote().nospace() << "lwIP assertion \"" << x << "\" failed at " << __FILE__ << ":" << __LINE__; \
} while(0)
#define LWIP_PLATFORM_LOG_USE_QDEBUG 1
#elif __has_include(<QtCore/QDebug>)
#include <QtCore/QDebug>
#define LWIP_PLATFORM_DIAG(x) do { qDebug().noquote().nospace() << x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    qCritical().noquote().nospace() << "lwIP assertion \"" << x << "\" failed at " << __FILE__ << ":" << __LINE__; \
} while(0)
#define LWIP_PLATFORM_LOG_USE_QDEBUG 1
#endif
#endif
#endif

#ifndef LWIP_PLATFORM_LOG_USE_QDEBUG
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    fprintf(stderr, "lwIP assertion \"%s\" failed at %s:%d\n", x, __FILE__, __LINE__); \
} while(0)
#endif

#endif /* LWIP_ARCH_CC_H */
