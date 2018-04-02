/*
 * (c) Copyright 2001, 2004 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _EMALLOC_H_
#define _EMALLOC_H_

#include <stdlib.h>

void *
emallocaligned(size_t size);
    
void *
emalloc(size_t size);

void *
erealloc(void *p,
	 size_t size);

char *
estrdup(const char str[]);

void
emalloc_set_exit_function(void (*exit_function)(int),
			  int status);

void
efree(void *p);

#endif
