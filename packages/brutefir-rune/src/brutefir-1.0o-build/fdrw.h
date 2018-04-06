/*
 * (c) Copyright 2000, 2001, 2002 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef FDRW_H_
#define FDRW_H_

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "defs.h"

static inline bool_t
readfd(int fd,
       void *buf,
       size_t count)
{
    int n, i = 0;

    do {
	if ((n = read(fd, &((uint8_t *)buf)[i], count - i)) < 1) {
	    if (n == 0 || errno != EINTR) {
		fprintf(stderr, "(%d) read from fd %d failed: %s\n",
                        (int)getpid(), fd, strerror(errno));
		return false;
	    }
	    continue;
	}
	i += n;
    } while (i != count);
    return true;
}

static inline bool_t
writefd(int fd,
	const void *buf,
	size_t count)
{
    int n, i = 0;

    do {
	if ((n = write(fd, &((const uint8_t *)buf)[i], count - i)) < 1) {
	    if (n == 0 || errno != EINTR) {
		fprintf(stderr, "(%d) write to fd %d failed: %s\n",
                        (int)getpid(), fd, strerror(errno));
		return false;
	    }
	    continue;
	}
	i += n;
    } while (i != count);
    return true;
}

#endif
