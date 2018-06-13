/*
 * (c) Copyright 2001 - 2003, 2006 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
static void
MIXNSCALE_NAME(void *input_cbufs[],
               void *output_cbuf,
               double scales[],
               int n_bufs,
               int mixmode)
{
    int n, i;
    real_t sscales[n_bufs], **ibufs = (real_t **)input_cbufs;
    real_t *obuf = (real_t *)output_cbuf;
    
    for (n = 0; n < n_bufs; n++) {
	sscales[n] = (real_t)scales[n];
    }    
    switch (mixmode) {
    case CONVOLVER_MIXMODE_INPUT:
	switch (n_bufs) {
	case 1:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+0] = ibufs[0][n+0] * sscales[0];
		obuf[(n<<1)+1] = ibufs[0][n+1] * sscales[0];
		obuf[(n<<1)+2] = ibufs[0][n+2] * sscales[0];
		obuf[(n<<1)+3] = ibufs[0][n+3] * sscales[0];
	    }
	    obuf[4] = ibufs[0][n_fft >> 1] * sscales[0];
	    ibufs[0] = &ibufs[0][n_fft];
	    obuf[5] = ibufs[0][-1] * sscales[0];
	    obuf[6] = ibufs[0][-2] * sscales[0];
	    obuf[7] = ibufs[0][-3] * sscales[0];
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+4] = ibufs[0][-n-0] * sscales[0];
		obuf[(n<<1)+5] = ibufs[0][-n-1] * sscales[0];
		obuf[(n<<1)+6] = ibufs[0][-n-2] * sscales[0];
		obuf[(n<<1)+7] = ibufs[0][-n-3] * sscales[0];
	    }
	    ibufs[0] = &ibufs[0][-n_fft];
	    break;
	case 2:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+0] =
		    ibufs[0][n+0] * sscales[0] +
		    ibufs[1][n+0] * sscales[1];
		obuf[(n<<1)+1] =
		    ibufs[0][n+1] * sscales[0] +
		    ibufs[1][n+1] * sscales[1];
		obuf[(n<<1)+2] =
		    ibufs[0][n+2] * sscales[0] +
		    ibufs[1][n+2] * sscales[1];
		obuf[(n<<1)+3] =
		    ibufs[0][n+3] * sscales[0] +
		    ibufs[1][n+3] * sscales[1];
	    }
	    obuf[4] =
		ibufs[0][n_fft >> 1] * sscales[0] +
		ibufs[1][n_fft >> 1] * sscales[1];
	    ibufs[0] = &ibufs[0][n_fft];
	    ibufs[1] = &ibufs[1][n_fft];
	    obuf[5] =
		ibufs[0][-1] * sscales[0] +
		ibufs[1][-1] * sscales[1];
	    obuf[6] =
		ibufs[0][-2] * sscales[0] +
		ibufs[1][-2] * sscales[1];
	    obuf[7] =
		ibufs[0][-3] * sscales[0] +
		ibufs[1][-3] * sscales[1];
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+4] =
		    ibufs[0][-n-0] * sscales[0] +
		    ibufs[1][-n-0] * sscales[1];
		obuf[(n<<1)+5] =
		    ibufs[0][-n-1] * sscales[0] +
		    ibufs[1][-n-1] * sscales[1];
		obuf[(n<<1)+6] =
		    ibufs[0][-n-2] * sscales[0] +
		    ibufs[1][-n-2] * sscales[1];
		obuf[(n<<1)+7] =
		    ibufs[0][-n-3] * sscales[0] +
		    ibufs[1][-n-3] * sscales[1];
	    }
	    ibufs[0] = &ibufs[0][-n_fft];
	    ibufs[1] = &ibufs[1][-n_fft];
	    break;
	case 3:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+0] =
		    ibufs[0][n+0] * sscales[0] +
		    ibufs[1][n+0] * sscales[1] +
		    ibufs[2][n+0] * sscales[2];
		obuf[(n<<1)+1] =
		    ibufs[0][n+1] * sscales[0] +
		    ibufs[1][n+1] * sscales[1] +
		    ibufs[2][n+1] * sscales[2];
		obuf[(n<<1)+2] =
		    ibufs[0][n+2] * sscales[0] +
		    ibufs[1][n+2] * sscales[1] +
		    ibufs[2][n+2] * sscales[2];
		obuf[(n<<1)+3] =
		    ibufs[0][n+3] * sscales[0] +
		    ibufs[1][n+3] * sscales[1] +
		    ibufs[2][n+3] * sscales[2];
	    }
	    obuf[4] =
		ibufs[0][n_fft >> 1] * sscales[0] +
		ibufs[1][n_fft >> 1] * sscales[1] +
		ibufs[2][n_fft >> 1] * sscales[2];
	    ibufs[0] = &ibufs[0][n_fft];
	    ibufs[1] = &ibufs[1][n_fft];
	    ibufs[2] = &ibufs[2][n_fft];
	    obuf[5] =
		ibufs[0][-1] * sscales[0] +
		ibufs[1][-1] * sscales[1] +
		ibufs[2][-1] * sscales[2];
	    obuf[6] =
		ibufs[0][-2] * sscales[0] +
		ibufs[1][-2] * sscales[1] +
		ibufs[2][-2] * sscales[2];
	    obuf[7] =
		ibufs[0][-3] * sscales[0] +
		ibufs[1][-3] * sscales[1] +
		ibufs[2][-3] * sscales[2];
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+4] =
		    ibufs[0][-n-0] * sscales[0] +
		    ibufs[1][-n-0] * sscales[1] +
		    ibufs[2][-n-0] * sscales[2];
		obuf[(n<<1)+5] =
		    ibufs[0][-n-1] * sscales[0] +
		    ibufs[1][-n-1] * sscales[1] +
		    ibufs[2][-n-1] * sscales[2];
		obuf[(n<<1)+6] =
		    ibufs[0][-n-2] * sscales[0] +
		    ibufs[1][-n-2] * sscales[1] +
		    ibufs[2][-n-2] * sscales[2];
		obuf[(n<<1)+7] =
		    ibufs[0][-n-3] * sscales[0] +
		    ibufs[1][-n-3] * sscales[1] +
		    ibufs[2][-n-3] * sscales[2];
	    }
	    ibufs[0] = &ibufs[0][-n_fft];
	    ibufs[1] = &ibufs[1][-n_fft];
	    ibufs[2] = &ibufs[2][-n_fft];
	    break;
	case 4:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+0] =
		    ibufs[0][n+0] * sscales[0] +
		    ibufs[1][n+0] * sscales[1] +
		    ibufs[2][n+0] * sscales[2] +
		    ibufs[3][n+0] * sscales[3];
		obuf[(n<<1)+1] =
		    ibufs[0][n+1] * sscales[0] +
		    ibufs[1][n+1] * sscales[1] +
		    ibufs[2][n+1] * sscales[2] +
		    ibufs[3][n+1] * sscales[3];
		obuf[(n<<1)+2] =
		    ibufs[0][n+2] * sscales[0] +
		    ibufs[1][n+2] * sscales[1] +
		    ibufs[2][n+2] * sscales[2] +
		    ibufs[3][n+2] * sscales[3];
		obuf[(n<<1)+3] =
		    ibufs[0][n+3] * sscales[0] +
		    ibufs[1][n+3] * sscales[1] +
		    ibufs[2][n+3] * sscales[2] +
		    ibufs[3][n+3] * sscales[3];
	    }
	    obuf[4] =
		ibufs[0][n_fft >> 1] * sscales[0] +
		ibufs[1][n_fft >> 1] * sscales[1] +
		ibufs[2][n_fft >> 1] * sscales[2] +
		ibufs[3][n_fft >> 1] * sscales[3];
	    ibufs[0] = &ibufs[0][n_fft];
	    ibufs[1] = &ibufs[1][n_fft];
	    ibufs[2] = &ibufs[2][n_fft];
	    ibufs[3] = &ibufs[3][n_fft];
	    obuf[5] =
		ibufs[0][-1] * sscales[0] +
		ibufs[1][-1] * sscales[1] +
		ibufs[2][-1] * sscales[2] +
		ibufs[3][-1] * sscales[3];
	    obuf[6] =
		ibufs[0][-2] * sscales[0] +
		ibufs[1][-2] * sscales[1] +
		ibufs[2][-2] * sscales[2] +
		ibufs[3][-2] * sscales[3];
	    obuf[7] =
		ibufs[0][-3] * sscales[0] +
		ibufs[1][-3] * sscales[1] +
		ibufs[2][-3] * sscales[2] +
		ibufs[3][-3] * sscales[3];
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+4] =
		    ibufs[0][-n-0] * sscales[0] +
		    ibufs[1][-n-0] * sscales[1] +
		    ibufs[2][-n-0] * sscales[2] +
		    ibufs[3][-n-0] * sscales[3];
		obuf[(n<<1)+5] =
		    ibufs[0][-n-1] * sscales[0] +
		    ibufs[1][-n-1] * sscales[1] +
		    ibufs[2][-n-1] * sscales[2] +
		    ibufs[3][-n-1] * sscales[3];
		obuf[(n<<1)+6] =
		    ibufs[0][-n-2] * sscales[0] +
		    ibufs[1][-n-2] * sscales[1] +
		    ibufs[2][-n-2] * sscales[2] +
		    ibufs[3][-n-2] * sscales[3];
		obuf[(n<<1)+7] =
		    ibufs[0][-n-3] * sscales[0] +
		    ibufs[1][-n-3] * sscales[1] +
		    ibufs[2][-n-3] * sscales[2] +
		    ibufs[3][-n-3] * sscales[3];
	    }
	    ibufs[0] = &ibufs[0][-n_fft];
	    ibufs[1] = &ibufs[1][-n_fft];
	    ibufs[2] = &ibufs[2][-n_fft];
	    ibufs[3] = &ibufs[3][-n_fft];
	    break;
	default:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+0] = ibufs[0][n+0] * sscales[0];
		obuf[(n<<1)+1] = ibufs[0][n+1] * sscales[0];
		obuf[(n<<1)+2] = ibufs[0][n+2] * sscales[0];
		obuf[(n<<1)+3] = ibufs[0][n+3] * sscales[0];
		for (i = 1; i < n_bufs; i++) {
		    obuf[(n<<1)+0] += ibufs[i][n+0] * sscales[i];
		    obuf[(n<<1)+1] += ibufs[i][n+1] * sscales[i];
		    obuf[(n<<1)+2] += ibufs[i][n+2] * sscales[i];
		    obuf[(n<<1)+3] += ibufs[i][n+3] * sscales[i];
		}
	    }
	    obuf[4] = ibufs[0][n_fft >> 1] * sscales[0];
	    ibufs[0] = &ibufs[0][n_fft];
	    obuf[5] = ibufs[0][-1] * sscales[0];
	    obuf[6] = ibufs[0][-2] * sscales[0];
	    obuf[7] = ibufs[0][-3] * sscales[0];
	    for (i = 1; i < n_bufs; i++) {
		obuf[4] += ibufs[i][n_fft >> 1] * sscales[i];
		ibufs[i] = &ibufs[i][n_fft];
		obuf[5] += ibufs[i][-1] * sscales[i];
		obuf[6] += ibufs[i][-2] * sscales[i];
		obuf[7] += ibufs[i][-3] * sscales[i];
	    }
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[(n<<1)+4] = ibufs[0][-n-0] * sscales[0];
		obuf[(n<<1)+5] = ibufs[0][-n-1] * sscales[0];
		obuf[(n<<1)+6] = ibufs[0][-n-2] * sscales[0];
		obuf[(n<<1)+7] = ibufs[0][-n-3] * sscales[0];
		for (i = 1; i < n_bufs; i++) {
		    obuf[(n<<1)+4] += ibufs[i][-n-0] * sscales[i];
		    obuf[(n<<1)+5] += ibufs[i][-n-1] * sscales[i];
		    obuf[(n<<1)+6] += ibufs[i][-n-2] * sscales[i];
		    obuf[(n<<1)+7] += ibufs[i][-n-3] * sscales[i];
		}
	    }
	    for (i = 0; i < n_bufs; i++) {
		ibufs[i] = &ibufs[i][-n_fft];
	    }
	    break;
	}
	break;
	
    case CONVOLVER_MIXMODE_OUTPUT:
	switch (n_bufs) {
	case 1:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[n+0] = ibufs[0][(n<<1)+0] * sscales[0];
		obuf[n+1] = ibufs[0][(n<<1)+1] * sscales[0];
		obuf[n+2] = ibufs[0][(n<<1)+2] * sscales[0];
		obuf[n+3] = ibufs[0][(n<<1)+3] * sscales[0];
	    }
	    obuf[n_fft >> 1] = ibufs[0][4] * sscales[0];
	    obuf = &obuf[n_fft];
	    obuf[-1] = ibufs[0][5] * sscales[0];
	    obuf[-2] = ibufs[0][6] * sscales[0];
	    obuf[-3] = ibufs[0][7] * sscales[0];
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[-n-0] = ibufs[0][(n<<1)+4] * sscales[0];
		obuf[-n-1] = ibufs[0][(n<<1)+5] * sscales[0];
		obuf[-n-2] = ibufs[0][(n<<1)+6] * sscales[0];
		obuf[-n-3] = ibufs[0][(n<<1)+7] * sscales[0];
	    }
	    break;
	case 2:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[n+0] =
		    ibufs[0][(n<<1)+0] * sscales[0] +
		    ibufs[1][(n<<1)+0] * sscales[1];
		obuf[n+1] =
		    ibufs[0][(n<<1)+1] * sscales[0] +
		    ibufs[1][(n<<1)+1] * sscales[1];
		obuf[n+2] =
		    ibufs[0][(n<<1)+2] * sscales[0] +
		    ibufs[1][(n<<1)+2] * sscales[1];
		obuf[n+3] =
		    ibufs[0][(n<<1)+3] * sscales[0] +
		    ibufs[1][(n<<1)+3] * sscales[1];
	    }
	    obuf[n_fft >> 1] =
		ibufs[0][4] * sscales[0] +
		ibufs[1][4] * sscales[1];
	    obuf = &obuf[n_fft];
	    obuf[-1] =
		ibufs[0][5] * sscales[0] +
		ibufs[1][5] * sscales[1];
	    obuf[-2] =
		ibufs[0][6] * sscales[0] +
		ibufs[1][6] * sscales[1];
	    obuf[-3] =
		ibufs[0][7] * sscales[0] +
		ibufs[1][7] * sscales[1];
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[-n-0] =
		    ibufs[0][(n<<1)+4] * sscales[0] +
		    ibufs[1][(n<<1)+4] * sscales[1];
		obuf[-n-1] =
		    ibufs[0][(n<<1)+5] * sscales[0] +
		    ibufs[1][(n<<1)+5] * sscales[1];
		obuf[-n-2] =
		    ibufs[0][(n<<1)+6] * sscales[0] +
		    ibufs[1][(n<<1)+6] * sscales[1];
		obuf[-n-3] =
		    ibufs[0][(n<<1)+7] * sscales[0] +
		    ibufs[1][(n<<1)+7] * sscales[1];
	    }
	    break;
	case 3:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[n+0] =
		    ibufs[0][(n<<1)+0] * sscales[0] +
		    ibufs[1][(n<<1)+0] * sscales[1] +
		    ibufs[2][(n<<1)+0] * sscales[2];
		obuf[n+1] =
		    ibufs[0][(n<<1)+1] * sscales[0] +
		    ibufs[1][(n<<1)+1] * sscales[1] +
		    ibufs[2][(n<<1)+1] * sscales[2];
		obuf[n+2] =
		    ibufs[0][(n<<1)+2] * sscales[0] +
		    ibufs[1][(n<<1)+2] * sscales[1] +
		    ibufs[2][(n<<1)+2] * sscales[2];
		obuf[n+3] =
		    ibufs[0][(n<<1)+3] * sscales[0] +
		    ibufs[1][(n<<1)+3] * sscales[1] +
		    ibufs[2][(n<<1)+3] * sscales[2];
	    }
	    obuf[n_fft >> 1] =
		ibufs[0][4] * sscales[0] +
		ibufs[1][4] * sscales[1] +
		ibufs[2][4] * sscales[2];
	    obuf = &obuf[n_fft];
	    obuf[-1] =
		ibufs[0][5] * sscales[0] +
		ibufs[1][5] * sscales[1] +
		ibufs[2][5] * sscales[2];
	    obuf[-2] =
		ibufs[0][6] * sscales[0] +
		ibufs[1][6] * sscales[1] +
		ibufs[2][6] * sscales[2];
	    obuf[-3] =
		ibufs[0][7] * sscales[0] +
		ibufs[1][7] * sscales[1] +
		ibufs[2][7] * sscales[2];
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[-n-0] =
		    ibufs[0][(n<<1)+4] * sscales[0] +
		    ibufs[1][(n<<1)+4] * sscales[1] +
		    ibufs[2][(n<<1)+4] * sscales[2];
		obuf[-n-1] =
		    ibufs[0][(n<<1)+5] * sscales[0] +
		    ibufs[1][(n<<1)+5] * sscales[1] +
		    ibufs[2][(n<<1)+5] * sscales[2];
		obuf[-n-2] =
		    ibufs[0][(n<<1)+6] * sscales[0] +
		    ibufs[1][(n<<1)+6] * sscales[1] +
		    ibufs[2][(n<<1)+6] * sscales[2];
		obuf[-n-3] =
		    ibufs[0][(n<<1)+7] * sscales[0] +
		    ibufs[1][(n<<1)+7] * sscales[1] +
		    ibufs[2][(n<<1)+7] * sscales[2];
	    }
	    break;
	case 4:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[n+0] =
		    ibufs[0][(n<<1)+0] * sscales[0] +
		    ibufs[1][(n<<1)+0] * sscales[1] +
		    ibufs[2][(n<<1)+0] * sscales[2] +
		    ibufs[3][(n<<1)+0] * sscales[3];
		obuf[n+1] =
		    ibufs[0][(n<<1)+1] * sscales[0] +
		    ibufs[1][(n<<1)+1] * sscales[1] +
		    ibufs[2][(n<<1)+1] * sscales[2] +
		    ibufs[3][(n<<1)+1] * sscales[3];
		obuf[n+2] =
		    ibufs[0][(n<<1)+2] * sscales[0] +
		    ibufs[1][(n<<1)+2] * sscales[1] +
		    ibufs[2][(n<<1)+2] * sscales[2] +
		    ibufs[3][(n<<1)+2] * sscales[3];
		obuf[n+3] =
		    ibufs[0][(n<<1)+3] * sscales[0] +
		    ibufs[1][(n<<1)+3] * sscales[1] +
		    ibufs[2][(n<<1)+3] * sscales[2] +
		    ibufs[3][(n<<1)+3] * sscales[3];
	    }
	    obuf[n_fft >> 1] =
		ibufs[0][4] * sscales[0] +
		ibufs[1][4] * sscales[1] +
		ibufs[2][4] * sscales[2] +
		ibufs[3][4] * sscales[3];
	    obuf = &obuf[n_fft];
	    obuf[-1] =
		ibufs[0][5] * sscales[0] +
		ibufs[1][5] * sscales[1] +
		ibufs[2][5] * sscales[2] +
		ibufs[3][5] * sscales[3];
	    obuf[-2] =
		ibufs[0][6] * sscales[0] +
		ibufs[1][6] * sscales[1] +
		ibufs[2][6] * sscales[2] +
		ibufs[3][6] * sscales[3];
	    obuf[-3] =
		ibufs[0][7] * sscales[0] +
		ibufs[1][7] * sscales[1] +
		ibufs[2][7] * sscales[2] +
		ibufs[3][7] * sscales[3];
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[-n-0] =
		    ibufs[0][(n<<1)+4] * sscales[0] +
		    ibufs[1][(n<<1)+4] * sscales[1] +
		    ibufs[2][(n<<1)+4] * sscales[2] +
		    ibufs[3][(n<<1)+4] * sscales[3];
		obuf[-n-1] =
		    ibufs[0][(n<<1)+5] * sscales[0] +
		    ibufs[1][(n<<1)+5] * sscales[1] +
		    ibufs[2][(n<<1)+5] * sscales[2] +
		    ibufs[3][(n<<1)+5] * sscales[3];
		obuf[-n-2] =
		    ibufs[0][(n<<1)+6] * sscales[0] +
		    ibufs[1][(n<<1)+6] * sscales[1] +
		    ibufs[2][(n<<1)+6] * sscales[2] +
		    ibufs[3][(n<<1)+6] * sscales[3];
		obuf[-n-3] =
		    ibufs[0][(n<<1)+7] * sscales[0] +
		    ibufs[1][(n<<1)+7] * sscales[1] +
		    ibufs[2][(n<<1)+7] * sscales[2] +
		    ibufs[3][(n<<1)+7] * sscales[3];
	    }
	    break;
	default:
	    for (n = 0; n < n_fft >> 1; n += 4) {
		obuf[n+0] = ibufs[0][(n<<1)+0] * sscales[0];
		obuf[n+1] = ibufs[0][(n<<1)+1] * sscales[0];
		obuf[n+2] = ibufs[0][(n<<1)+2] * sscales[0];
		obuf[n+3] = ibufs[0][(n<<1)+3] * sscales[0];
		for (i = 1; i < n_bufs; i++) {
		    obuf[n+0] += ibufs[i][(n<<1)+0] * sscales[i];
		    obuf[n+1] += ibufs[i][(n<<1)+1] * sscales[i];
		    obuf[n+2] += ibufs[i][(n<<1)+2] * sscales[i];
		    obuf[n+3] += ibufs[i][(n<<1)+3] * sscales[i];
		}
	    }
	    obuf[n_fft >> 1] = ibufs[0][4] * sscales[0];
	    for (i = 1; i < n_bufs; i++) {
		obuf[n_fft >> 1] += ibufs[i][4] * sscales[i];
	    }
	    obuf = &obuf[n_fft];
	    obuf[-1] = ibufs[0][5] * sscales[0];
	    obuf[-2] = ibufs[0][6] * sscales[0];
	    obuf[-3] = ibufs[0][7] * sscales[0];
	    for (i = 1; i < n_bufs; i++) {
		obuf[-1] += ibufs[i][5] * sscales[i];
		obuf[-2] += ibufs[i][6] * sscales[i];
		obuf[-3] += ibufs[i][7] * sscales[i];
	    }
	    for (n = 4; n < n_fft >> 1; n += 4) {
		obuf[-n-0] = ibufs[0][(n<<1)+4] * sscales[0];
		obuf[-n-1] = ibufs[0][(n<<1)+5] * sscales[0];
		obuf[-n-2] = ibufs[0][(n<<1)+6] * sscales[0];
		obuf[-n-3] = ibufs[0][(n<<1)+7] * sscales[0];
		for (i = 1; i < n_bufs; i++) {
		    obuf[-n-0] += ibufs[i][(n<<1)+4] * sscales[i];
		    obuf[-n-1] += ibufs[i][(n<<1)+5] * sscales[i];
		    obuf[-n-2] += ibufs[i][(n<<1)+6] * sscales[i];
		    obuf[-n-3] += ibufs[i][(n<<1)+7] * sscales[i];
		}
	    }
	    break;
	}
	break;
	
    default:
	fprintf(stderr, "Invalid mixmode: %d.\n", mixmode);
	bf_exit(BF_EXIT_OTHER);
	break;
    }
}

static void
CONVOLVE_INPLACE_NAME(void *cbuf,
                      void *coeffs)
{
    int n;
    real_t a[4];
    real_t *b = (real_t *)cbuf;
    real_t *c = (real_t *)coeffs;
    real_t d1s, d2s;

    d1s = b[0] * c[0];
    d2s = b[4] * c[4];
    for (n = 0; n < n_fft; n += 8) {
	a[0] = b[n+0];
	a[1] = b[n+1];
	a[2] = b[n+2];
	a[3] = b[n+3];
	b[n+0] = a[0] * c[n+0] - b[n+4] * c[n+4];
	b[n+1] = a[1] * c[n+1] - b[n+5] * c[n+5];
	b[n+2] = a[2] * c[n+2] - b[n+6] * c[n+6];
	b[n+3] = a[3] * c[n+3] - b[n+7] * c[n+7];
	
    	b[n+4] = a[0] * c[n+4] + b[n+4] * c[n+0];
    	b[n+5] = a[1] * c[n+5] + b[n+5] * c[n+1];
    	b[n+6] = a[2] * c[n+6] + b[n+6] * c[n+2];
    	b[n+7] = a[3] * c[n+7] + b[n+7] * c[n+3];
    }
    b[0] = d1s;
    b[4] = d2s;
}

static void
CONVOLVE_NAME(void *input_cbuf,
              void *coeffs,
              void *output_cbuf)
{    
    int n;
    real_t *b = (real_t *)input_cbuf;
    real_t *c = (real_t *)coeffs;
    real_t *d = (real_t *)output_cbuf;
    real_t d1s, d2s;

    d1s = b[0] * c[0];
    d2s = b[4] * c[4];
    for (n = 0; n < n_fft; n += 8) {
	d[n+0] = b[n+0] * c[n+0] - b[n+4] * c[n+4];
	d[n+1] = b[n+1] * c[n+1] - b[n+5] * c[n+5];
	d[n+2] = b[n+2] * c[n+2] - b[n+6] * c[n+6];
	d[n+3] = b[n+3] * c[n+3] - b[n+7] * c[n+7];
	
    	d[n+4] = b[n+0] * c[n+4] + b[n+4] * c[n+0];
    	d[n+5] = b[n+1] * c[n+5] + b[n+5] * c[n+1];
    	d[n+6] = b[n+2] * c[n+6] + b[n+6] * c[n+2];
    	d[n+7] = b[n+3] * c[n+7] + b[n+7] * c[n+3];
    }
    d[0] = d1s;
    d[4] = d2s;

}


static void
CONVOLVE_ADD_NAME(void *input_cbuf,
                  void *coeffs,
                  void *output_cbuf)
{
    real_t *b = (real_t *)input_cbuf;
    real_t *c = (real_t *)coeffs;
    real_t *d = (real_t *)output_cbuf;
    real_t d1s, d2s;
    int n;
    
    d1s = d[0] + b[0] * c[0];
    d2s = d[4] + b[4] * c[4];
    for (n = 0; n < n_fft; n += 8) {
        d[n+0] += b[n+0] * c[n+0] - b[n+4] * c[n+4];
        d[n+1] += b[n+1] * c[n+1] - b[n+5] * c[n+5];
        d[n+2] += b[n+2] * c[n+2] - b[n+6] * c[n+6];
        d[n+3] += b[n+3] * c[n+3] - b[n+7] * c[n+7];
        
        d[n+4] += b[n+0] * c[n+4] + b[n+4] * c[n+0];
        d[n+5] += b[n+1] * c[n+5] + b[n+5] * c[n+1];
        d[n+6] += b[n+2] * c[n+6] + b[n+6] * c[n+2];
        d[n+7] += b[n+3] * c[n+7] + b[n+7] * c[n+3];
    }
    d[0] = d1s;
    d[4] = d2s;
}

static void
DIRAC_CONVOLVE_INPLACE_NAME(void *cbuf)
{
    real_t fraction = 1.0 / (real_t)n_fft;
    int n;

    for (n = 0; n < n_fft; n += 4) {
	((real_t *)cbuf)[n+0] *= +fraction;
	((real_t *)cbuf)[n+1] *= -fraction;
	((real_t *)cbuf)[n+2] *= +fraction;
	((real_t *)cbuf)[n+3] *= -fraction;
    }
}

static void
DIRAC_CONVOLVE_NAME(void *input_cbuf,
                    void *output_cbuf)
{
    real_t fraction = 1.0 / (real_t)n_fft;
    int n;

    for (n = 0; n < n_fft; n += 4) {
	((real_t *)output_cbuf)[n+0] = ((real_t *)input_cbuf)[n+0] * +fraction;
	((real_t *)output_cbuf)[n+1] = ((real_t *)input_cbuf)[n+1] * -fraction;
	((real_t *)output_cbuf)[n+2] = ((real_t *)input_cbuf)[n+2] * +fraction;
	((real_t *)output_cbuf)[n+3] = ((real_t *)input_cbuf)[n+3] * -fraction;
    }
}

