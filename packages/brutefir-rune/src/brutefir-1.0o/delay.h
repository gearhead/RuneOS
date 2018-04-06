/*
 * (c) Copyright 2002, 2006 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _DELAY_H_
#define _DELAY_H_

#include "defs.h"

typedef struct _delaybuffer_t_ delaybuffer_t;

/* optional_target_buf has sample_spacing == 1 */
void
delay_update(delaybuffer_t *db,
	     void *buf,	     
	     int sample_size,
	     int sample_spacing,
	     int delay,
	     void *optional_target_buf);

delaybuffer_t *
delay_allocate_buffer(int fragment_size,
		      int initdelay,
		      int maxdelay,
		      int sample_size);

int
delay_subsample_filterblocksize(void);

void
delay_subsample_update(void *buf,
                       void *rest,
                       int subdelay);

bool_t
delay_subsample_init(int step_count,
                     int half_filter_length,
                     double kaiser_beta,
                     int fragment_size,
                     int _realsize);

#endif
