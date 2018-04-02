/*
 * (c) Copyright 2001 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _PINFO_H_
#define _PINFO_H_

#include "bfconf.h"

#define pinfo(format, args...) if (!bfconf->quiet)                             \
   fprintf(stderr, format, ##args);

#endif
