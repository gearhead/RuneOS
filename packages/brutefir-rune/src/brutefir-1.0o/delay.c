/*
 * (c) Copyright 2002, 2004, 2006 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "defs.h"
#include "pinfo.h"
#include "delay.h"
#include "bfrun.h"
#include "emalloc.h"
#include "firwindow.h"
#include "convolver.h"
#include "timestamp.h"

static int realsize;
static int subdelay_filter_length;
static td_conv_t **subdelay_filter = NULL;
static int subdelay_step_count;
static int subdelay_filterblock_size;
static int subdelay_fragment_size;

struct _delaybuffer_t_ {
    int fragsize;    /* fragment size */
    int maxdelay;    /* maximum allowable delay, or negative if delay cannot be
			changed in runtime */
    int curdelay;    /* current delay */
    int curbuf;      /* index of current full-sized buffer */
    int n_fbufs;     /* number of full-sized buffers currently used */
    int n_fbufs_cap; /* number of full-sized buffers allocated */
    void **fbufs;    /* full-sized buffers */
    int n_rest;      /* samples in rest buffer */
    void *rbuf;      /* rest buffer */
    void *shortbuf[2]; /* pointers to buffers which fit the whole delay, only
                          used when delay is <= fragment size */
};

static double
sinc(double x)
{
    if (x == 0.0) {
        return 1.0;
    }
    return sin(x) / x;
}

static void *
sample_sinc(int half_length,
            double offset,
            double kaiser_beta)
{
    int n, filter_length;
    void *filter;
    double x;

    filter_length = 2 * half_length + 1;
    filter = emallocaligned(filter_length * realsize);
    for (n = 0; n < filter_length; n++) {
        x = M_PI * ((double)(n - half_length) - offset);
        if (realsize == 4) {
            ((float *)filter)[n] = (float)sinc(x);
        } else {
            ((double *)filter)[n] = sinc(x);
        }
    }
    firwindow_kaiser(filter, filter_length, offset, 9, realsize);
    return filter;
}

static void
copy_to_delaybuf(void *dbuf,
		 void *buf,
		 int sample_size,
		 int sample_spacing,
		 int n_samples)
{
    int n, i;
    
    if (sample_spacing == 1) {
	memcpy(dbuf, buf, n_samples * sample_size);
	return;
    }
    
    switch (sample_size) {
    case 1:
	for (n = i = 0; n < n_samples; n++, i += sample_spacing) {
	    ((uint8_t *)dbuf)[n] = ((uint8_t *)buf)[i];
	}	
	break;
    case 2:
	for (n = i = 0; n < n_samples; n++, i += sample_spacing) {
	    ((uint16_t *)dbuf)[n] = ((uint16_t *)buf)[i];
	}	
	break;
    case 3:
	n_samples *= 3;
	sample_spacing = sample_spacing * 3 - 3;
	for (n = i = 0; n < n_samples; i += sample_spacing) {
	    ((uint8_t *)dbuf)[n++] = ((uint8_t *)buf)[i++];
	    ((uint8_t *)dbuf)[n++] = ((uint8_t *)buf)[i++];
	    ((uint8_t *)dbuf)[n++] = ((uint8_t *)buf)[i++];
	}	
	break;
    case 4:
	for (n = i = 0; n < n_samples; n++, i += sample_spacing) {
	    ((uint32_t *)dbuf)[n] = ((uint32_t *)buf)[i];
	}		
	break;
    case 8:
	for (n = i = 0; n < n_samples; n++, i += sample_spacing) {
	    ((uint64_t *)dbuf)[n] = ((uint64_t *)buf)[i];
	}		
	break;
    default:
	fprintf(stderr, "Sample byte size %d not supported.\n", sample_size);
	bf_exit(BF_EXIT_OTHER);
	break;
    }
}

static void
copy_from_delaybuf(void *buf,
		   void *dbuf,		   
		   int sample_size,
		   int sample_spacing,
		   int n_samples)
{
    int n, i;

    if (sample_spacing == 1) {
	memcpy(buf, dbuf, n_samples * sample_size);
	return;
    }
    
    switch (sample_size) {
    case 1:
	for (n = i = 0; n < n_samples; n++, i += sample_spacing) {
	    ((uint8_t *)buf)[i] = ((uint8_t *)dbuf)[n];
	}	
	break;
    case 2:
	for (n = i = 0; n < n_samples; n++, i += sample_spacing) {
	    ((uint16_t *)buf)[i] = ((uint16_t *)dbuf)[n];
	}	
	break;
    case 3:
	n_samples *= 3;
	sample_spacing = sample_spacing * 3 - 3;
	for (n = i = 0; n < n_samples; i += sample_spacing) {
	    ((uint8_t *)buf)[i++] = ((uint8_t *)dbuf)[n++];
	    ((uint8_t *)buf)[i++] = ((uint8_t *)dbuf)[n++];
	    ((uint8_t *)buf)[i++] = ((uint8_t *)dbuf)[n++];
	}	
	break;
    case 4:
	for (n = i = 0; n < n_samples; n++, i += sample_spacing) {
	    ((uint32_t *)buf)[i] = ((uint32_t *)dbuf)[n];
	}		
	break;
    case 8:
	for (n = i = 0; n < n_samples; n++, i += sample_spacing) {
	    ((uint64_t *)buf)[i] = ((uint64_t *)dbuf)[n];
	}		
	break;
    default:
	fprintf(stderr, "Sample byte size %d not supported.\n", sample_size);
	bf_exit(BF_EXIT_OTHER);
	break;
    }
}

static void
shift_samples(void *buf,
	      int sample_size,
	      int sample_spacing,
	      int n_samples,
	      int distance)
{
    int n, i;

    n = (n_samples - 1) * sample_spacing;
    i = (n_samples + distance - 1) * sample_spacing;
    switch (sample_size) {
    case 1:
	for (; n >= 0; n -= sample_spacing, i -= sample_spacing) {
	    ((uint8_t *)buf)[i] = ((uint8_t *)buf)[n];
	}	
	break;
    case 2:
	for (; n >= 0; n -= sample_spacing, i -= sample_spacing) {
	    ((uint16_t *)buf)[i] = ((uint16_t *)buf)[n];
	}	
	break;
    case 3:
	n *= 3;
	i *= 3;
	sample_spacing *= 3;
	for (; n >= 0; n -= sample_spacing, i -= sample_spacing) {
	    ((uint8_t *)buf)[i++] = ((uint8_t *)buf)[n++];
	    ((uint8_t *)buf)[i++] = ((uint8_t *)buf)[n++];
	    ((uint8_t *)buf)[i++] = ((uint8_t *)buf)[n++];
	}	
	break;
    case 4:
	for (; n >= 0; n -= sample_spacing, i -= sample_spacing) {
	    ((uint32_t *)buf)[i] = ((uint32_t *)buf)[n];
	}	
	break;
    case 8:
	for (; n >= 0; n -= sample_spacing, i -= sample_spacing) {
	    ((uint64_t *)buf)[i] = ((uint64_t *)buf)[n];
	}	
	break;
    default:
	fprintf(stderr, "Sample byte size %d not supported.\n", sample_size);
	bf_exit(BF_EXIT_OTHER);
	break;
    }    
}

static void
update_delay_buffer(delaybuffer_t *db,
		    int sample_size,
		    int sample_spacing,
		    uint8_t *buf)
{
    uint8_t *lastbuf;

    lastbuf = (db->curbuf == db->n_fbufs - 1) ? db->fbufs[0] :
	db->fbufs[db->curbuf + 1];

    /* 1. copy buffer to current full-sized delay buffer */
    copy_to_delaybuf(db->fbufs[db->curbuf], buf, sample_size,
		     sample_spacing, db->fragsize);
    
    if (db->n_rest != 0) {
	/* 2. copy from delay rest buffer to the start of buffer */
	copy_from_delaybuf(buf, db->rbuf, sample_size, sample_spacing,
			   db->n_rest);
    
	/* 3. copy from end of last full-sized delay buffer to rest buffer */
	memcpy(db->rbuf, lastbuf + (db->fragsize - db->n_rest) * sample_size,
	       db->n_rest * sample_size);
    }

    /* 4. copy from start of last full-sized buffer to end of buffer */
    copy_from_delaybuf(buf + db->n_rest * sample_size * sample_spacing,
		       lastbuf, sample_size, sample_spacing,
		       db->fragsize - db->n_rest);
    
    if (++db->curbuf == db->n_fbufs) {
	db->curbuf = 0;
    }
}

static void
update_delay_short_buffer(delaybuffer_t *db,
			  int sample_size,
			  int sample_spacing,
			  uint8_t *buf)
{
    copy_to_delaybuf(db->shortbuf[db->curbuf], 
		     buf + (db->fragsize - db->n_rest) * sample_size *
		     sample_spacing, sample_size, sample_spacing,
		     db->n_rest);

    shift_samples(buf, sample_size, sample_spacing,
		  db->fragsize - db->n_rest, db->n_rest);
    
    db->curbuf = !db->curbuf;
    copy_from_delaybuf(buf, db->shortbuf[db->curbuf], sample_size,
		       sample_spacing, db->n_rest);
}

static void
change_delay(delaybuffer_t *db,
	     int sample_size,
	     int newdelay)
{
    int size, i;

    if (newdelay == db->curdelay || newdelay > db->maxdelay) {
	return;
    }
    if (newdelay <= db->fragsize) {
	db->n_rest = newdelay;
	size = newdelay * sample_size;
	if (db->curdelay > db->fragsize || db->curdelay < newdelay) {
	    memset(db->shortbuf[0], 0, size);
	    memset(db->shortbuf[1], 0, size);
	}
	db->n_fbufs = 0;
	db->curbuf = 0;
	db->curdelay = newdelay;
	return;
    }
    db->n_rest = newdelay % db->fragsize;
    db->n_fbufs = newdelay / db->fragsize + 1;
    size = db->fragsize * sample_size;
    if (db->curdelay < newdelay) {
	for (i = 0; i < db->n_fbufs; i++) {
	    memset(db->fbufs[i], 0, size);
	}
	if (db->n_rest != 0) {
	    memset(db->rbuf, 0, db->n_rest * sample_size);
	}
    }
    db->curbuf = 0;
    db->curdelay = newdelay;
}

void
delay_update(delaybuffer_t *db,
	     void *buf,	     
	     int sample_size,
	     int sample_spacing,
	     int delay,
	     void *optional_target_buf)
{
    change_delay(db, sample_size, delay);
    if (optional_target_buf != NULL) {
	copy_to_delaybuf(optional_target_buf, buf, sample_size, sample_spacing,
			 db->fragsize);
	buf = optional_target_buf;
	sample_spacing = 1;
    }
    if (db->n_fbufs > 0) {
	update_delay_buffer(db, sample_size, sample_spacing, buf);
    } else if (db->n_rest > 0) {
	update_delay_short_buffer(db, sample_size, sample_spacing, buf);
    }
}

delaybuffer_t *
delay_allocate_buffer(int fragment_size,
		      int initdelay,
		      int maxdelay,
		      int sample_size)
{
    delaybuffer_t *db;
    int n, delay;
    int size;

    /* if maxdelay is negative, no delay changing will be allowed, thus
       memory need only to be allocated for the current delay */
    db = emalloc(sizeof(delaybuffer_t));
    memset(db, 0, sizeof(delaybuffer_t));
    db->fragsize = fragment_size;
    
    delay = (maxdelay <= 0) ? initdelay : maxdelay;
    if (maxdelay >= 0 && delay > maxdelay) {
	delay = initdelay = maxdelay;
    }
    db->curdelay = initdelay;
    db->maxdelay = maxdelay;	
    if (delay == 0) {
	return db;
    }
    if (delay <= fragment_size) {
	/* optimise for short delay */
	db->n_rest = initdelay; /* current value */
	size = delay * sample_size;
	db->shortbuf[0] = emallocaligned(size);
	db->shortbuf[1] = emallocaligned(size);
	memset(db->shortbuf[0], 0, size);
	memset(db->shortbuf[1], 0, size);
	return db;
    }
    if (maxdelay > 0) {
	/* allocate full-length short buffers to keep this option if the
	   delay is reduced in run-time */
	size = fragment_size * sample_size;
	db->shortbuf[0] = emallocaligned(size);
	db->shortbuf[1] = emallocaligned(size);
	memset(db->shortbuf[0], 0, size);
	memset(db->shortbuf[1], 0, size);
    }
    
    db->n_rest = initdelay % fragment_size;
    db->n_fbufs = initdelay / fragment_size + 1;
    if (db->n_fbufs == 1) {
	db->n_fbufs = 0;
    }
    db->n_fbufs_cap = delay / fragment_size + 1;
    db->fbufs = emalloc(db->n_fbufs_cap * sizeof(void *));
    size = fragment_size * sample_size;
    for (n = 0; n < db->n_fbufs_cap; n++) {
	db->fbufs[n] = emallocaligned(size);
	memset(db->fbufs[n], 0, size);
    }
    if (maxdelay > 0) {
	db->rbuf = emallocaligned(size);
	memset(db->rbuf, 0, size);
    } else if (db->n_rest != 0) {
	size = db->n_rest * sample_size;
	db->rbuf = emallocaligned(size);
	memset(db->rbuf, 0, size);
    }
    return db;
}

int
delay_subsample_filterblocksize(void)
{
    return subdelay_filterblock_size;
}

void
delay_subsample_update(void *buf,
                       void *rest,
                       int subdelay)
{
    void *cbuf_low, *cbuf_high, *cbuffer;
    int i, blocksize;
    uint64_t t1, t2;

    if (subdelay <= -subdelay_step_count || subdelay >= subdelay_step_count) {
        return;
    }
    timestamp(&t1);
    blocksize = subdelay_filterblock_size * realsize;
    cbuffer = alloca(blocksize << 1);
    cbuf_low = cbuffer;
    cbuf_high = &((uint8_t *)cbuf_low)[blocksize];
    for (i = 0; i < subdelay_fragment_size * realsize; i += blocksize) {
        memcpy(cbuf_low, rest, blocksize);
        memcpy(cbuf_high, &((uint8_t *)buf)[i], blocksize);
        memcpy(rest, cbuf_high, blocksize);
        convolver_td_convolve(subdelay_filter[subdelay], cbuffer);
        memcpy(&((uint8_t *)buf)[i], cbuf_low, blocksize);
    }
    timestamp(&t2);
    t2 -= t1;
    /*fprintf(stderr, "%" PRIu64 "\n", t2 / (uint64_t)bfconf->cpu_mhz);*/
}

bool_t
delay_subsample_init(int step_count,
                     int half_filter_length,
                     double kaiser_beta,
                     int fragment_size,
                     int _realsize)
{
    void *filter;
    int n;

    realsize = _realsize;
    subdelay_filter_length = 2 * half_filter_length + 1;
    subdelay_filterblock_size =
        convolver_td_block_length(subdelay_filter_length);
    if (step_count < 2) {
        fprintf(stderr, "Invalid step_count %d.\n", step_count);
        return false;
    }
    if (half_filter_length < 1) {
	fprintf(stderr, "Invalid half filter length %d.\n", half_filter_length);
	return false;
    }
    if (fragment_size % subdelay_filterblock_size != 0) {
	fprintf(stderr, "Incompatible fragment/filter sizes (%d/%d).\n",
                fragment_size, subdelay_filter_length);
	return false;
    }
    if (realsize != 4 && realsize != 8) {
	fprintf(stderr, "Invalid real size %d.\n", realsize);
	return false;
    }
    subdelay_fragment_size = fragment_size;
    subdelay_step_count = step_count;
    subdelay_filter = emalloc((2 * step_count + 1) * sizeof(td_conv_t *));
    subdelay_filter = &subdelay_filter[step_count];
    filter = emalloc(subdelay_filter_length * realsize);
    memset(filter, 0, subdelay_filter_length * realsize);
    if (realsize == 4) {
        ((float *)filter)[subdelay_filter_length >> 1] = 1.0;
    } else {
        ((double *)filter)[subdelay_filter_length >> 1] = 1.0;
    }
    subdelay_filter[0] = convolver_td_new(filter, subdelay_filter_length);
    efree(filter);
    for (n = 1; n < step_count; n++) {
        filter = sample_sinc(subdelay_filter_length >> 1,
                             (double)n / step_count,
                             kaiser_beta);
        subdelay_filter[n] = convolver_td_new(filter, subdelay_filter_length);
        efree(filter);
    }
    for (n = -1; n > -step_count; n--) {
        filter = sample_sinc(subdelay_filter_length >> 1,
                             (double)n / step_count,
                             kaiser_beta);
        subdelay_filter[n] = convolver_td_new(filter, subdelay_filter_length);
        efree(filter);
    }
    pinfo("Created %d subsample delay filters with %d taps "
          "and kaiser beta %g.\n",
          2 * step_count - 1, subdelay_filter_length, kaiser_beta);
    return true;
}
