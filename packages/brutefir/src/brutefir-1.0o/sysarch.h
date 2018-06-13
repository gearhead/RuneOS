/*
 * (c) Copyright 2002 - 2004, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _SYSARCH_H_
#define _SYSARCH_H_

#define ALIGNMENT 32

/*
 * Find out CPU architecture
 */
#if defined(__i386__) || defined(_M_IX86)
#define __ARCH_IA32__
#define __LITTLE_ENDIAN__
#elif defined(__sparc__) || defined(__sparc)
#define __ARCH_SPARC__
#define __BIG_ENDIAN__
#elif defined(__x86_64__)
#define __ARCH_X86_64__
#define __LITTLE_ENDIAN__
#else
#define __ARCH_GENERIC__
#include <endian.h>
#if __BYTE_ORDER == __LITTLE_ENDIAN
#undef __LITTLE_ENDIAN__
#define __LITTLE_ENDIAN__
#endif
#if __BYTE_ORDER == __BIG_ENDIAN
#undef __BIG_ENDIAN__
#define __BIG_ENDIAN__
#endif
#endif

#if (defined(__LITTLE_ENDIAN__) && defined(__BIG_ENDIAN__)) ||                 \
    (!defined(__LITTLE_ENDIAN__) && !defined(__BIG_ENDIAN__))
 #error unknown byte order
#endif

/*
 * Find out OS
 */
#ifdef linux
#define __OS_LINUX__
#elif defined(__SVR4) && defined(__sun)
#define __OS_SUNOS__
#elif __FreeBSD__
#define __OS_FREEBSD__
#else
#define __OS_GENERIC__
#endif


#endif
