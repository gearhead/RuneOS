/*
 * (c) Copyright 2001 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _LOG2_H_
#define _LOG2_H_

#include <inttypes.h>

/* FIXME: these implementations does not look very elegant */

static inline int
log2_get(uint32_t x)
{
    int lg;
    
    for (lg = 0; (x & 1) == 0 && lg < 32; x = x >> 1, lg++);
    if (lg == 32 || (x & ~1) != 0) {
	return -1;
    }
    return lg;    
}

static inline int
log2_roof(uint32_t x)
{
    int lg;
    
    for (lg = 31; (x & (1 << lg)) == 0 && lg > 0; lg--);
    if (lg == 0 || (lg == 31 && (x & 0x7FFFFFFF) != 0)) {
	return -1;
    }
    if ((x & ~(1 << lg)) != 0) {
	lg++;
    }
    return lg;
}

#endif
