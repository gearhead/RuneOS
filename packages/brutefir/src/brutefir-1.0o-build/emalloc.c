/*
 * (c) Copyright 2001 - 2004, 2009, 2016 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "defs.h"
#include "emalloc.h"

static void (*exit_func)(int) = NULL;
static int exit_status = 1;

#define EXIT_PROGRAM if (exit_func == NULL) exit(exit_status);                 \
                     else exit_func(exit_status);

#define PRINT_MESSAGE                                                          \
    fprintf(stderr, "Memory allocation failure (%d bytes), "                   \
            "terminating program.\n", (int)size);

static void
check_avail(size_t alloc)
{
#ifdef __OS_LINUX__
    int memtotal, memfree, buffers, cached, allocsize, fill;
    FILE *stream;
    char s[100];

    /* on any error, just return ok */
    if ((stream = fopen("/proc/meminfo", "rt")) == NULL) {
        return;
    }
    allocsize = alloc / 1024;
    memtotal = memfree = buffers = cached = 0;
    s[sizeof(s)-1] = '\0';
    while (fgets(s, sizeof(s)-1, stream) != NULL) {
        if (memtotal == 0 && strstr(s, "MemTotal:") == s) {
            memtotal = atoi(&s[9]);
        } else if (memfree == 0 && strstr(s, "MemFree:") == s) {
            memfree = atoi(&s[8]);
        } else if (buffers == 0 && strstr(s, "Buffers:") == s) {
            buffers = atoi(&s[8]);
        } else if (cached == 0 && strstr(s, "Cached:") == s) {
            cached = atoi(&s[7]);
        }
        if (memtotal != 0 && memfree != 0 && buffers != 0 && cached != 0) {
            break;
        }
    }
    fclose(stream);
    if (memtotal == 0 || memfree == 0) {
        return;
    }
    fill = 100 * (memtotal - memfree - buffers - cached + allocsize) / memtotal;
    if (fill > 90) {
        fprintf(stderr, "Too much (%d%%) of the available memory is allocated, "
                "exiting\n", fill);
        EXIT_PROGRAM;
    }
#endif    
}

void *
emallocaligned(size_t size)
{
    int err;
    void *p = NULL;

    if (size == 0) {
        return NULL;
    }
    check_avail(size);
#if defined(__OS_LINUX__)
#if (__GLIBC__ < 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 3)
    /* use old memalign, posix_memalign may be buggy in these glibc versions */
    p = memalign(ALIGNMENT, size < ALIGNMENT ? ALIGNMENT : size);
    err = !p;
#else
    err = posix_memalign(&p, ALIGNMENT, size < ALIGNMENT ? ALIGNMENT : size);
#endif
#elif defined(__OS_FREEBSD__)
    if (size < getpagesize()) {
        size = getpagesize();
    }
    if (ALIGNMENT > size) {
        fprintf(stderr,
                "ALIGNMENT (%d) is larger than the pagesize (%d), aborting.\n"
                "  Recompile with a smaller alignment.\n",
                (int)ALIGNMENT, (int)getpagesize());
        EXIT_PROGRAM;
    }
    p = malloc(size);
    err = !p;
#else
    p = memalign(ALIGNMENT, size < ALIGNMENT ? ALIGNMENT : size);
    err = !p;
#endif
    
    if (err != 0) {
	PRINT_MESSAGE;
	EXIT_PROGRAM;
    }
    return p;
}

void *
emalloc(size_t size)
{
    void *p;

    if (size == 0) {
        return NULL;
    }
    check_avail(size);
    p = malloc(size);
    if (p == NULL) {
	PRINT_MESSAGE;
	EXIT_PROGRAM;
    }
    return p;
}

void *
erealloc(void *p,
	 size_t size)
{
    p = realloc(p, size);
    if (size > 0) {
        check_avail(0);
    }
    if (p == NULL) {
	PRINT_MESSAGE;
	EXIT_PROGRAM;
    }
    return p;
}

char *
estrdup(const char str[])
{
    size_t size = strlen(str) + 1;
    char *newstr = emalloc(size);
    memcpy(newstr, str, size);
    return newstr;
}

void
emalloc_set_exit_function(void (*exit_function)(int),
			  int status)
{
    exit_func = exit_function;
    exit_status = status;
}

void
efree(void *p)
{
    free(p);
}
