/*
 * (c) Copyright 2001, 2003 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _BFRUN_H_
#define _BFRUN_H_

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>

#include "defs.h"
#include "bfmod.h"
#include "dither.h"
#include "convolver.h"

struct filter_process {
    int n_unique_channels[2];
    int *unique_channels[2];
    int n_filters;    
    struct bffilter *filters;
};

void
bfrun(void);

void
bf_callback_ready(int io);

void
bf_reset_peak(void);

void
bf_register_process(pid_t pid);

void
bf_exit(int status);

double
bf_realtime_index(void);

void
bf_make_realtime(pid_t pid,
                 int priority,
                 const char name[]);

int
bflogic_command(int modindex,
                const char params[],
                char **message);

const char **
bflogic_names(int *n_names);

const char **
bfio_names(int io,
	   int *n_names);

void
bfio_range(int io,
	   int modindex,
	   int range[2]);

#endif
