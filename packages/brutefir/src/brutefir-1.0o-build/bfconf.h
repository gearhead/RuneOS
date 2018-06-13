/*
 * (c) Copyright 2001 - 2004, 2006, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _BFCONF_H_
#define _BFCONF_H_

#include <stdlib.h>
#include <inttypes.h>

#include "defs.h"
#include "dai.h"
#include "bfrun.h"
#include "bfmod.h"
#include "dither.h"
#include "timestamp.h"

#define DEFAULT_BFCONF_NAME "~/.brutefir_defaults"

struct bfconf {
    double cpu_mhz;
    int n_cpus;
    int sampling_rate;
    int filter_length;
    int n_blocks;
    int max_dither_table_size;
    int flowthrough_blocks;
    int realtime_maxprio;
    int realtime_midprio;
    int realtime_usermaxprio;
    int realtime_minprio;
    int realsize;    
    bool_t callback_io;
    bool_t blocking_io;
    bool_t powersave;
    double analog_powersave;
    bool_t benchmark;
    bool_t debug;
    bool_t quiet;
    bool_t overflow_warnings;
    bool_t show_progress;
    bool_t realtime_priority;
    bool_t lock_memory;
    bool_t monitor_rate;
    bool_t synched_write;
    bool_t allow_poll_mode;
    struct dither_state **dither_state;
    int n_coeffs;
    struct bfcoeff *coeffs;
    void ***coeffs_data;
    int n_channels[2];
    struct bfchannel *channels[2];
    int n_physical_channels[2];
    int *n_virtperphys[2];
    int **phys2virt[2];
    int *virt2phys[2];
    int n_subdevs[2];
    struct dai_subdevice *subdevs[2];
    int *delay[2];
    int *maxdelay[2];
    bool_t *mute[2];
    int n_filters;
    struct bffilter *filters;
    struct bffilter_control *initfctrl;
    int n_processes;
    struct filter_process *fproc;
    int n_iomods;
    struct bfio_module *iomods;
    char **ionames;
    int n_logicmods;
    struct bflogic_module *logicmods;
    char **logicnames;
    bool_t use_subdelay[2];
    int *subdelay[2];
    int sdf_length;
    double sdf_beta;
    double safety_limit;
};

extern struct bfconf *bfconf;

void
bfconf_init(char filename[],
	    bool_t quiet,
            bool_t nodefault);

#endif
