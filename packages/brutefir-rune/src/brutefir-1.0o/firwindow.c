/*
 * (c) Copyright 2002 - 2003, 2006 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdlib.h>
#include <math.h>
#include <assert.h>

#include "firwindow.h"

/* zeroth order modified bessel function */
static double
i_zero(double x)
{
    /*
      The zeroth order Modified Bessel function:

      I_0(x) = 1 + 
        x^2 / 2^2 + 
        x^4 / (2^2 * 4^2) +
        x^6 / (2^2 * 4^2 * 6^2) +
        ... to infinity        


      This function grows quite quickly towards very large numbers.
      By re-organising the calculations somewhat we minimise the dynamic
      range in the floating point numbers, and can thus calculate the
      function for larger x than we could do with a naive implementation.
      
     */
    double n, a, halfx, sum;
    halfx = x / 2.0;
    sum = 1.0;
    a = 1.0;
    n = 1.0;
    do {
        a *= halfx;
        a /= n;
        sum += a * a;
        n += 1.0;
        /* either 'sum' will reach +inf or 'a' zero... */
    } while (a != 0.0 && finite(sum));
    return sum;
}

static double
kaiser(double x,
       double beta,
       double inv_izbeta) /* inv_izbeta = 1.0 / i_zero(beta) */
{
    /*
      The Kaiser Window, with discrete n on odd length window:
      
      w(n) = I_0(Beta * sqrt(1 - 4*n^2 / (N-1)^2)) / I_0(Beta)
      with -(N-1)/2 <= n <= (N-1)/2
      I_0 is the zeroth order modified bessel function.

      modified to -1.0 <= x <= x:

      y(x) = I_O(Beta * sqrt(1 - x^2)) / I_0(Beta)

     */
    
    /* check input parameter x, assume, accept and correct possible
       rounding error. */
    if (x < -1.0) {
        assert(x > -1.00001);
        x = -1.0;
    }
    if (x > 1.0) {
        assert(x < +1.00001);
        x = 1.0;
    }
    return i_zero(beta * sqrt(1.0 - x * x)) * inv_izbeta;
}

void
firwindow_kaiser(void *target,
                 int len,
                 double offset,
                 double beta,
                 int realsize)
{
    int n, len_div2, max;
    double x, y, inv_izbeta, step;

    len_div2 = len >> 1;
    inv_izbeta = 1.0 / i_zero(beta);
    if (offset != 0.0) {
        max = len_div2;
        max += (int)floor(offset);
        offset -= floor(offset);
        if (fabs(offset) < 1e-20) {
            offset = 0.0;
        }
        step = 1.0 / ((double)max + offset);
        if (offset == 0.0) {
            /* if offset is a whole number, don't run the loop up to zero
               to avoid unnecessary rounding errors on zero */
            max -= 1;
        }
        for (n = 0; n <= max; n++) {
            x = -1.0 + (double)n * step;
            y = kaiser(x, beta, inv_izbeta);
            if (realsize == 4) {
                ((float *)target)[n] *= y;
                ((float *)target)[n] *= y;
            } else {
                ((double *)target)[n] *= y;
                ((double *)target)[n] *= y;
            }
        }
        if (offset == 0.0) {
            max += 1;
        }
        step = 1.0 / ((double)(len - max - 1) - offset);
        for (; n < len; n++) {
            x = ((double)(n - max) - offset) * step;
            y = kaiser(x, beta, inv_izbeta);
            if (realsize == 4) {
                ((float *)target)[n] *= y;
                ((float *)target)[n] *= y;
            } else {
                ((double *)target)[n] *= y;
                ((double *)target)[n] *= y;
            }
        }
    } else if ((len & 1) != 0) {
        /* odd length, center is the middle sample; this is the 'standard'
           case for windowing functions */
        step = 1.0 / (double)len_div2;
        for (n = 1; n <= len_div2; n++) {
            x = (double)n * step;
            y = kaiser(x, beta, inv_izbeta);
            if (realsize == 4) {
                ((float *)target)[len_div2 + n] *= y;
                ((float *)target)[len_div2 - n] *= y;
            } else {
                ((double *)target)[len_div2 + n] *= y;
                ((double *)target)[len_div2 - n] *= y;
            }
        }
    } else {
        /* even length, center is in between the two middle samples; this
           case is equivalent to even length and offset = '-0.5'. */
        step = 1.0 / (double)len_div2;
        step *= ((double)(len_div2) / ((double)len_div2 - 0.5));
        for (n = 1; n <= len_div2; n++) {
            x = ((double)n - 0.5) * step;
            y = kaiser(x, beta, inv_izbeta);
            if (realsize == 4) {
                ((float *)target)[len_div2 + n - 1] *= y;
                ((float *)target)[len_div2 - n] *= y;
            } else {
                ((double *)target)[len_div2 + n - 1] *= y;
                ((double *)target)[len_div2 - n] *= y;
            }
        }
    }
}
