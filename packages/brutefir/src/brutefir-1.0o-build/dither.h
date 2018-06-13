/*
 * (c) Copyright 2001, 2003 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _DITHER_H_
#define _DITHER_H_

#include <inttypes.h>

#include "defs.h"
#include "dai.h"
#include "bfmod.h"
#include "convolver.h"

struct dither_state {
    int randtab_ptr;
    int8_t *randtab;
    float sf[2];
    double sd[2];
};

extern int8_t *dither_randtab;
extern int dither_randtab_size;
extern void *dither_randmap;

static inline void
dither_preloop_real2int_hp_tpdf(struct dither_state *state,
				int samples_per_loop)
{
    if (state->randtab_ptr + samples_per_loop >= dither_randtab_size) {
	dither_randtab[0] = dither_randtab[state->randtab_ptr - 1];
	state->randtab_ptr = 1;
    }
    state->randtab = &dither_randtab[state->randtab_ptr];
    state->randtab_ptr += samples_per_loop;
}

#define real_t float
#define REALSIZE 4
#define REAL2INT_HP_TPDF_NAME ditherf_real2int_hp_tpdf
#define REAL2INT_NO_DITHER_NAME ditherf_real2int_no_dither
#include "dither_funs.h"
#undef REAL2INT_HP_TPDF_NAME
#undef REAL2INT_NO_DITHER_NAME
#undef REALSIZE
#undef real_t

#define real_t double
#define REALSIZE 8
#define REAL2INT_HP_TPDF_NAME ditherd_real2int_hp_tpdf
#define REAL2INT_NO_DITHER_NAME ditherd_real2int_no_dither
#include "dither_funs.h"
#undef REAL2INT_HP_TPDF_NAME
#undef REAL2INT_NO_DITHER_NAME
#undef REALSIZE
#undef real_t

bool_t
dither_init(int n_channels,
	    int sample_rate,
            int realsize,
	    int max_size,
	    int max_samples_per_loop,
	    struct dither_state *dither_states[]);

#endif
