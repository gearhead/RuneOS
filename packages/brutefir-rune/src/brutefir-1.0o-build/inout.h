/*
 * (c) Copyright 2001 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _INOUT_H_
#define _INOUT_H_

#include "bfmod.h"

#define IN  BF_IN
#define OUT BF_OUT

#define FOR_IN_AND_OUT for (IO = 0; IO < 2; IO++)

extern int IO;

#endif
