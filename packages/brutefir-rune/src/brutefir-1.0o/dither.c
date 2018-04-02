/*
 * (c) Copyright 2001, 2003 - 2004 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "defs.h"
#include "dai.h"
#include "dither.h"
#include "shmalloc.h"
#include "emalloc.h"
#include "pinfo.h"

/* desired spacing between channels in random number table in seconds */
#define RANDTAB_SPACING 10
#define MIN_RANDTAB_SPACING 1

int8_t *dither_randtab;
int dither_randtab_size;
void *dither_randmap = NULL;

static int realsize;

/*
 * This is a maximally equidistributed combined Tausworthe generator, stolen
 * from GNU Scientific Library (GSL). See GSL documentation for further
 * information.
 *
 * Generates numbers between 0x0 - 0xFFFFFFFF
 */
static inline uint32_t
tausrand(uint32_t state[3])
{
#define TAUSWORTHE(s,a,b,c,d) ((s & c) << d) ^ (((s <<a) ^ s) >> b)  

  state[0] = TAUSWORTHE(state[0], 13, 19, (uint32_t)4294967294U, 12);
  state[1] = TAUSWORTHE(state[1], 2, 25, (uint32_t)4294967288U, 4);
  state[2] = TAUSWORTHE(state[2], 3, 11, (uint32_t)4294967280U, 17);

  return (state[0] ^ state[1] ^ state[2]);
}

static void
tausinit(uint32_t state[3],
	 uint32_t seed)
{
  /* default seed is 1 */
  if (seed == 0) {
      seed = 1; 
  }
 
#define LCG(n) ((69069 * n) & 0xFFFFFFFFU)
  
  state[0] = LCG(seed);
  state[1] = LCG(state[0]);
  state[2] = LCG(state[1]);
 
  /* "warm it up" */
  tausrand(state);
  tausrand(state);
  tausrand(state);
  tausrand(state);
  tausrand(state);
  tausrand(state);
}                   

bool_t
dither_init(int n_channels,
	    int sample_rate,
            int _realsize,
	    int max_size,
	    int max_samples_per_loop,
	    struct dither_state *dither_states[])
{
    int n, spacing = RANDTAB_SPACING * sample_rate, minspacing;
    uint32_t state[3];

    realsize = _realsize;
    minspacing = (MIN_RANDTAB_SPACING * sample_rate > max_samples_per_loop) ?
	MIN_RANDTAB_SPACING * sample_rate : max_samples_per_loop;
    if (spacing < minspacing) {
	spacing = minspacing;
    }
    if (max_size > 0) {
	if (n_channels * spacing > max_size) {
	    spacing = max_size / n_channels;
	}
    }
    if (spacing < minspacing) {
	fprintf(stderr, "Maximum dither table size %d bytes is too small, must "
		"at least be %d bytes.\n", max_size,
		n_channels * sample_rate * minspacing);
	return false;
    }
    dither_randtab_size = n_channels * spacing + 1;
    
    pinfo("Dither table size is %d bytes.\n"
	  "Generating random numbers...", dither_randtab_size);
    tausinit(state, 0);
    dither_randtab = emallocaligned(dither_randtab_size);
    for (n = 0; n < dither_randtab_size; n++) {
	dither_randtab[n] = (int8_t)(tausrand(state) & 0x000000FF);
    }
    pinfo("finished.\n");

    /* make a map for conversion of integer dither random numbers to
       floating point ranging from -1.0 to +1.0, plus an offset of +0.5,
       used to make the sample truncation be mid-tread requantisation */
    dither_randmap = emallocaligned(realsize * 511);
    dither_randmap = &((uint8_t *)dither_randmap)[256 * realsize];
    if (realsize == 4) {
        ((float *)dither_randmap)[-256] = -0.5;
        for (n = -255; n < 254; n++) {
            ((float *)dither_randmap)[n] =
                0.5 + 1.0 / 255.0 + 1.0 / 255.0 * (float)n;
        }
        ((float *)dither_randmap)[254] = 1.5;
    } else {
        ((double *)dither_randmap)[-256] = -0.5;
        for (n = -255; n < 254; n++) {
            ((double *)dither_randmap)[n] =
                0.5 + 1.0 / 255.0 + 1.0 / 255.0 * (double)n;
        }
        ((double *)dither_randmap)[254] = 1.5;
    }

    for (n = 0; n < n_channels; n++) {
	dither_states[n] = emalloc(sizeof(struct dither_state));
	memset(dither_states[n], 0, sizeof(struct dither_state));
	dither_states[n]->randtab_ptr = n * spacing + 1;
    }
    return true;
}
