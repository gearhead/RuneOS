/*
 * (c) Copyright 2001, 2003 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _SHMALLOC_H_
#define _SHMALLOC_H_

void *
shmalloc(size_t size);

void *
shmalloc_id(int *shmid,
            size_t size);

#endif
