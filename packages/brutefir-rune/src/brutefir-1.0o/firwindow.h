/*
 * (c) Copyright 2002 - 2003, 2006 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _FIRWINDOW_H_
#define _FIRWINDOW_H_

#include "defs.h"

/****
 * firwindow_kaiser()
 *
 * Apply kaiser window with given 'beta' to 'target' of length 'len'.
 * For odd length targets, it is assumed that the center of the filter is
 * the middle sample (index len >> 1). For even length targets, it is assumed
 * that the center is exactly in between the two middle samples, and thus the
 * center is not sampled.
 *
 * If the center of 'target' is not placed as described above, a non-zero
 * offset can be specified with 'offset', which is the distance in samples
 * from sample index len >> 1.
 *
 * Note: due to the assumption of the center position in an even length
 * filter is the case when 'offset' is 0.0 equivalent to the case when it is
 * set to -0.5 (for even length filters only).
 *
 * 'beta' is a parameter for the kaiser window which specifies its shape.
 * A typical value suitable for general audio processing is 9.0.
 */
void
firwindow_kaiser(void *target,
                 int len,
                 double offset,
                 double beta,
                 int realsize);

#endif
