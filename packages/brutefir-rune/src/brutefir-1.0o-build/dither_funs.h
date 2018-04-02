/*
 * (c) Copyright 2001, 2003 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
static inline int32_t
REAL2INT_HP_TPDF_NAME(real_t real_sample,
                      real_t rmin, /* (real_t)imin */
                      real_t rmax, /* (real_t)imax */
                      int32_t imin,
                      int32_t imax,
                      struct bfoverflow *overflow,
                      struct dither_state *state,
                      int loop_counter)
{
    int32_t sample;
    real_t dithered_sample;

    /* apply error feedback, with coefficients {1, -1} (high pass) */
#if REALSIZE == 4
    real_sample += state->sf[0] - state->sf[1];   
    state->sf[1] = state->sf[0];
#else
    real_sample += state->sd[0] - state->sd[1];   
    state->sd[1] = state->sd[0];
#endif

    /* apply dither and offset */
    dithered_sample = real_sample +
	((real_t *)dither_randmap)[state->randtab[loop_counter] -
                                  state->randtab[loop_counter - 1]];
    
    if (dithered_sample < 0) {
	if (dithered_sample <= rmin) {
	    sample = imin;
	    overflow->n_overflows++;
            if (real_sample < -overflow->largest) {
                overflow->largest = (double)-dithered_sample;
            }
	} else {
	    sample = (int32_t)dithered_sample;
	    sample--;
            if (sample < -overflow->intlargest) {
                overflow->intlargest = -sample;
            }
	}
    } else {
	if (dithered_sample > rmax) {
	    sample = imax;
	    overflow->n_overflows++;
            if (real_sample > overflow->largest) {
                overflow->largest = (double)dithered_sample;
            }
	} else {
	    sample = (int32_t)dithered_sample;		
            if (sample > overflow->intlargest) {
                overflow->intlargest = sample;
            }
	}
    }
#if REALSIZE == 4
    state->sf[0] = real_sample - (real_t)sample;
#else    
    state->sd[0] = real_sample - (real_t)sample;
#endif    
    return sample;    
}

static inline int32_t
REAL2INT_NO_DITHER_NAME(real_t real_sample,
                        real_t rmin, /* (real_t)imin */
                        real_t rmax, /* (real_t)imax */
                        int32_t imin,
                        int32_t imax,
                        struct bfoverflow *overflow)
{
    int32_t sample;

    /*
     * Truncation is here made downwards, meaning 3.8 -> 3 and -3.2 -> -4. By
     * adding 0.5 before our truncation we get a mid-tread requantiser.
     */    
    real_sample += 0.5;
    if (real_sample < 0) {
	if (real_sample <= rmin) {
	    sample = imin;
	    overflow->n_overflows++;
            if (real_sample < -overflow->largest) {
                overflow->largest = (double)-real_sample;
            }
	} else {
	    sample = (int32_t)real_sample;
	    sample--;
            if (sample < -overflow->intlargest) {
                overflow->intlargest = -sample;
            }
	}
    } else {
	if (real_sample > rmax) {
	    sample = imax;
	    overflow->n_overflows++;
            if (real_sample > overflow->largest) {
                overflow->largest = (double)real_sample;
            }
	} else {
	    sample = (int32_t)real_sample;
            if (sample > overflow->intlargest) {
                overflow->intlargest = sample;
            }
	}
    }
    return sample;
}
