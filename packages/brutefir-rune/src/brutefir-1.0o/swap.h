/*
 * (c) Copyright 1999, 2002 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _SWAP_H_
#define _SWAP_H_

#include <inttypes.h>

#ifdef __LITTLE_ENDIAN__

#include <netinet/in.h> 

#define SWAP32(x) ntohl((uint32_t)(x))
#define SWAP16(x) ntohs((uint16_t)(x))

#endif
#ifdef __BIG_ENDIAN__

#define SWAP16(x)                                                              \
    (((((uint16_t)(x)) & 0xff00U) >> 8) |                                      \
     ((((uint16_t)(x)) & 0x00ffU) << 8))

#define SWAP32(x)                                                              \
    (((((uint32_t)(x)) & 0xff000000UL) >> 24) |                                \
     ((((uint32_t)(x)) & 0x00ff0000UL) >>  8) |                                \
     ((((uint32_t)(x)) & 0x0000ff00UL) <<  8) |                                \
     ((((uint32_t)(x)) & 0x000000ffUL) << 24))

#endif

#define SWAP64(x)                                                              \
    (((((uint64_t)(x)) & 0xff00000000000000ULL) >> 56) |                       \
     ((((uint64_t)(x)) & 0x00ff000000000000ULL) >> 40) |                       \
     ((((uint64_t)(x)) & 0x0000ff0000000000ULL) >> 24) |                       \
     ((((uint64_t)(x)) & 0x000000ff00000000ULL) >> 8)  |                       \
     ((((uint64_t)(x)) & 0x00000000ff000000ULL) << 8)  |                       \
     ((((uint64_t)(x)) & 0x0000000000ff0000ULL) << 24) |                       \
     ((((uint64_t)(x)) & 0x000000000000ff00ULL) << 40) |                       \
     ((((uint64_t)(x)) & 0x00000000000000ffULL) << 56))

#endif
