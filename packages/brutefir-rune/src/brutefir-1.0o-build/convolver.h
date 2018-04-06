/*
 * (c) Copyright 2001, 2002, 2004, 2006 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _CONVOLVER_H_
#define _CONVOLVER_H_

#include <inttypes.h>

#include "defs.h"
#include "bfmod.h"
#include "dai.h"

/* Convert from raw sample format to the convolver's own time-domain format. */
void
convolver_raw2cbuf(void *rawbuf,
		   void *cbuf,
		   void *next_cbuf,
		   struct buffer_format *bf,
                   void (*postprocess)(void *realbuf,
                                       int n_samples,
                                       void *arg),
                   void *pp_arg);

/* Transform from time-domain to frequency-domain. */
void
convolver_time2freq(void *input_cbuf,
		    void *output_cbuf);

/* Scale and mix in the frequency-domain. The 'mixmode' parameter may be used
   internally for possible reordering of data prior to or after convolution. */
void
convolver_mixnscale(void *input_cbufs[],
		    void *output_cbuf,
		    double scales[],
		    int n_bufs,
#define CONVOLVER_MIXMODE_INPUT      1
#define CONVOLVER_MIXMODE_INPUT_ADD  2
#define CONVOLVER_MIXMODE_OUTPUT     3
		    int mixmode);

/* Convolution in the frequency-domain, done in-place. */
void
convolver_convolve_inplace(void *cbuf,
			   void *coeffs);

/* Convolution in the frequency-domain. */
void
convolver_convolve(void *input_cbuf,
		   void *coeffs,
		   void *output_cbuf);

void
convolver_crossfade_inplace(void *input_cbuf,
                            void *crossfade_cbuf,
                            void *buffer_cbuf);

/* Convolution in the frequency-domain, with the result added to the output. */
void
convolver_convolve_add(void *input_cbuf,
		       void *coeffs,
		       void *output_cbuf);

/* Convolve with dirac pulse. */
void
convolver_dirac_convolve(void *input_cbuf,
			 void *output_cbuf);
void
convolver_dirac_convolve_inplace(void *cbuf);

/* Transform from frequency-domain to time-domain. */
void
convolver_freq2time(void *input_cbuf,
		    void *output_cbuf);

/* Evaluate convolution output by transforming it back to time-domain, do
   overlap-save and transform back to frequency-domain again. Used when filters
   are put in series. The 'buffer_cbuf' must be 1.5 times the cbufsize and must
   be cleared the first call, and be kept the following calls. Input and output
   buffers may be the same. */
void
convolver_convolve_eval(void *input_cbuf,
			void *buffer_cbuf,
			void *output_cbuf);

/* Convert from the convolver's own time-domain format to raw sample format. */
void
convolver_cbuf2raw(void *cbuf,
		   void *outbuf,
		   struct buffer_format *bf,
		   bool_t apply_dither,
		   void *dither_state,
		   struct bfoverflow *overflow);

/* Return the size of the convolver's internal format corresponding to the
   given number of samples. */
int
convolver_cbufsize(void);

/* Convert a set of coefficients to the convolver's internal frequency domain
   format. */
void *
convolver_coeffs2cbuf(void *coeffs,
		      int n_coeffs,
		      double scale,
                      void *optional_dest);

/* Fast version of the above to be used in runtime */
void
convolver_runtime_coeffs2cbuf(void *src,
                              void *dest);


/* Make a quick sanity check */
bool_t
convolver_verify_cbuf(void *cbufs[],
                      int n_cbufs);

/* Dump the contents of a cbuf to a text-file (after conversion back to
   coefficient list) */
void
convolver_debug_dump_cbuf(const char filename[],
                          void *cbufs[],
                          int n_cbufs);

/* Get a plan for the FFT lib used. Do not free it. */
void *
convolver_fftplan(int order,
                  int invert,
                  int inplace);


typedef struct _td_conv_t_ td_conv_t;

int
convolver_td_block_length(int n_coeffs);

td_conv_t *
convolver_td_new(void *coeffs,
                 int n_coeffs);

void
convolver_td_convolve(td_conv_t *tdc,
                      void *overlap_block);

/* Initialise convolver. Some convolvers may ignore 'config_filename' */
bool_t
convolver_init(const char config_filename[],
	       int length,
               int realsize);

#endif
