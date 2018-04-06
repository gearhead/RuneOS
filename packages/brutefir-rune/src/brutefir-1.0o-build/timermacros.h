/*
 * (c) Copyright 2002, 2003 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _TIMERMACROS_H_
#define _TIMERMACROS_H_

/* These two macros are defined in sys/time.h on most systems */
#include <sys/time.h>

#ifndef timeradd
#define timeradd(a, b, result)                                                 \
  do {                                                                         \
      (result)->tv_sec = (a)->tv_sec + (b)->tv_sec;                            \
      (result)->tv_usec = (a)->tv_usec + (b)->tv_usec;                         \
      if ((result)->tv_usec >= 1000000) {                                      \
          (result)->tv_sec++;                                                  \
          (result)->tv_usec -= 1000000;                                        \
      }                                                                        \
  } while (0)
#endif
#ifndef timersub
#define timersub(a, b, result)                                                 \
  do {                                                                         \
      (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;                            \
      (result)->tv_usec = (a)->tv_usec - (b)->tv_usec;                         \
      if ((result)->tv_usec < 0) {                                             \
          (result)->tv_sec--;                                                  \
          (result)->tv_usec += 1000000;                                        \
      }                                                                        \
  } while (0)
#endif
#ifndef timercmp
#define timercmp(a, b, CMP) 						       \
  (((a)->tv_sec == (b)->tv_sec) ? 					       \
   ((a)->tv_usec CMP (b)->tv_usec) : 					       \
   ((a)->tv_sec CMP (b)->tv_sec))
#endif

#endif
