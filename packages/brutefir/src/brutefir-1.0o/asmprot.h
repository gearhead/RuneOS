/*
 * (c) Copyright 2001, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _ASMPROT_H_
#define _ASMPROT_H_

void
convolver_sse_convolve_add(void *input_cbuf,
			   void *coeffs,
			   void *output_cbuf,
			   int loop_counter);

void
convolver_sse2_convolve_add(void *input_cbuf,
                            void *coeffs,
                            void *output_cbuf,
                            int loop_counter);

void
convolver_3dnow_convolve_add(void *input_cbuf,
			     void *coeffs,
			     void *output_cbuf,
			     int loop_counter);

#endif
