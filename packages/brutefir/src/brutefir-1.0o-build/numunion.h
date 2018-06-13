/*
 * (c) Copyright 2004 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _NUMUNION_H_
#define _NUMUNION_H_

#include <inttypes.h>

typedef union {
    float r32[2];
    double r64[1];
    int8_t i8[8];
    int16_t i16[4];
    int32_t i32[2];
    int64_t i64[1];
    uint8_t u8[8];
    uint16_t u16[4];
    uint32_t u32[2];
    uint64_t u64[1];
} numunion_t;

#endif
