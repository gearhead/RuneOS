/*
 * (c) Copyright 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include "asmprot.h"

#include <xmmintrin.h>

void
convolver_sse_convolve_add(void *input_cbuf,
			   void *coeffs,
			   void *output_cbuf,
			   int loop_counter)
{
    __m128 *b = (__m128 *)input_cbuf;
    __m128 *c = (__m128 *)coeffs;
    __m128 *d = (__m128 *)output_cbuf;
    float d1s, d2s;
    int i;

    d1s = ((float *)d)[0] + ((float *)b)[0] * ((float *)c)[0];
    d2s = ((float *)d)[4] + ((float *)b)[4] * ((float *)c)[4];
    for (i = 0; i < loop_counter; i++) {
        int n = i << 1;
        d[n+0] = _mm_add_ps(d[n+0], _mm_sub_ps(_mm_mul_ps(b[n+0], c[n+0]), _mm_mul_ps(b[n+1], c[n+1])));

        d[n+1] = _mm_add_ps(d[n+1], _mm_add_ps(_mm_mul_ps(b[n+0], c[n+1]), _mm_mul_ps(b[n+1], c[n+0])));
    }
    ((float *)d)[0] = d1s;
    ((float *)d)[4] = d2s;
}

#ifdef __SSE2__

void
convolver_sse2_convolve_add(void *input_cbuf,
                            void *coeffs,
                            void *output_cbuf,
                            int loop_counter)
{
    __m128d *b = (__m128d *)input_cbuf;
    __m128d *c = (__m128d *)coeffs;
    __m128d *d = (__m128d *)output_cbuf;
    double d1s, d2s;
    int i;

    d1s = ((double *)d)[0] + ((double *)b)[0] * ((double *)c)[0];
    d2s = ((double *)d)[4] + ((double *)b)[4] * ((double *)c)[4];
    for (i = 0; i < loop_counter; i++) {
        int n = i << 2;

        d[n+0] = _mm_add_pd(d[n+0], _mm_sub_pd(_mm_mul_pd(b[n+0], c[n+0]), _mm_mul_pd(b[n+2], c[n+2])));
        d[n+1] = _mm_add_pd(d[n+1], _mm_sub_pd(_mm_mul_pd(b[n+1], c[n+1]), _mm_mul_pd(b[n+3], c[n+3])));

        d[n+2] = _mm_add_pd(d[n+2], _mm_add_pd(_mm_mul_pd(b[n+0], c[n+2]), _mm_mul_pd(b[n+2], c[n+0])));
        d[n+3] = _mm_add_pd(d[n+3], _mm_add_pd(_mm_mul_pd(b[n+1], c[n+3]), _mm_mul_pd(b[n+3], c[n+1])));
    }
    ((double *)d)[0] = d1s;
    ((double *)d)[4] = d2s;
}

#endif
