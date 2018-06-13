/*
 * (c) Copyright 2000, 2002, 2004, 2006, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _TIMESTAMP_H_
#define _TIMESTAMP_H_

#include <inttypes.h>
#include <time.h>

#if defined(__ARCH_GENERIC__)
#define TIMESTAMP_NOT_CLOCK_CYCLE_COUNTER
static inline void
timestamp(volatile uint64_t * volatile ts)
{
    *ts = (uint64_t)clock() * 1000;
}
#endif

#if defined(__ARCH_IA32__)
#include <x86intrin.h>
static inline void
timestamp(volatile uint64_t * volatile ts)
{
    *ts = __rdtsc();
    /*__asm__ volatile("rdtsc" : "=A"(*ts));*/
}
#endif

#if defined(__ARCH_X86_64__)
#include <x86intrin.h>
static inline void
timestamp(volatile uint64_t * volatile ts)
{
    *ts = __rdtsc();
    /*__asm__ volatile("rdtsc":"=a"(*ts)::"dx");*/
}
#endif

#ifdef __ARCH_SPARC__
static inline void
timestamp(volatile uint64_t *ts)
{
    __asm__ volatile (
	"rd %%tick, %0    \n\t \
	 clruw %0, %1     \n\t \
	 srlx %0, 32, %0  \n\t"
	: "=r"(((volatile uint32_t *)ts)[0]), "=r"(((volatile uint32_t *)ts)[1])
	: "0"(((volatile uint32_t *)ts)[0]), "1"(((volatile uint32_t *)ts)[1]));
}
#endif

#endif
