/*
 * (c) Copyright 2001 - 2004, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */

#if REALSIZE == 4
#define RXX r32
#define REAL_T float
#elif REALSIZE == 8
#define RXX r64
#define REAL_T double
#else
 #error invalid REALSIZE
#endif        

#ifndef CONCAT_EVAL_
#define CONCAT_EVAL_(a, b) a ## b
#define CONCAT_(a, b) CONCAT_EVAL_(a, b)
#endif
#define REAL2RAW_SAMPLE_TEST CONCAT_(REAL2RAW_NAME, _sample_test)

static inline void
REAL2RAW_SAMPLE_TEST(REAL_T real_sample, struct bfoverflow *overflow)
{
    if (!isfinite(real_sample)) {
        fprintf(stderr, "NaN or Inf values in the output! Bad output. "
                        "Aborting.\n");
        abort();
    }
    if (bfconf->safety_limit != 0.0 &&
        (real_sample < -bfconf->safety_limit * overflow->max ||
         real_sample > bfconf->safety_limit * overflow->max))
    {
        fprintf(stderr, "Safety limit exceeded on output (%.2f > %.2f). "
                "Aborting.\n",
                20.0 * log10(fabs(real_sample / overflow->max)),
                20.0 * log10(bfconf->safety_limit));
        bf_exit(BF_EXIT_OTHER);
    }
}

#define REAL_OVERFLOW_UPDATE                                                   \
    if (realbuf->RXX[n] < 0.0) {                                               \
        if (realbuf->RXX[n] < rmin) {                                          \
            overflow->n_overflows++;                                           \
        }                                                                      \
        if (realbuf->RXX[n] < -overflow->largest) {                            \
            overflow->largest = -realbuf->RXX[n];                              \
        }                                                                      \
    } else {                                                                   \
        if (realbuf->RXX[n] > rmax) {                                          \
            overflow->n_overflows++;                                           \
        }                                                                      \
        if (realbuf->RXX[n] > overflow->largest) {                             \
            overflow->largest = realbuf->RXX[n];                               \
        }                                                                      \
    }

static void
REAL2RAW_NAME(void *_rawbuf,
	      void *_realbuf,
	      int bits,
	      int bytes,
	      bool_t isfloat,
	      int spacing,
	      bool_t swap,
	      int n_samples,
	      struct bfoverflow *overflow REAL2RAW_EXTRA_PARAMS)
{
    numunion_t *rawbuf, *realbuf, sample;
    int32_t imin, imax;
    REAL_T rmin, rmax;
    int n, i;

    /*
     * It is assumed that sbytes only can have the values possible from the
     * supported sample formats specified in bfmod.h
     */
    
    realbuf = (numunion_t *)_realbuf;
    rawbuf = (numunion_t *)_rawbuf;
    if (isfloat) {
        rmin = -overflow->max;
        rmax = overflow->max;
#if REALSIZE == 4
        switch (bytes) {
        case 4:
            if (swap) {
                for (n = i = 0; n < n_samples; n++, i += spacing) {
                    REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                    REAL_OVERFLOW_UPDATE;
                    rawbuf->u32[i] = SWAP32(realbuf->u32[n]);
                }
            } else {
                for (n = i = 0; n < n_samples; n++, i += spacing) {
                    REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                    REAL_OVERFLOW_UPDATE;
                    rawbuf->r32[i] = realbuf->r32[n];
                }
            }
            break;
        case 8:
            if (swap) {
                for (n = i = 0; n < n_samples; n++, i += spacing) {
                    REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                    REAL_OVERFLOW_UPDATE;
                    sample.r64[0] = (double)realbuf->r32[n];
                    rawbuf->u64[i] = SWAP64(sample.u64[0]);
                }
            } else {
                for (n = i = 0; n < n_samples; n++, i += spacing) {
                    REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                    REAL_OVERFLOW_UPDATE;
                    rawbuf->r64[i] = (double)realbuf->r32[n];
                }
            }
            break;
        default:
            goto real2raw_invalid_byte_size;
        }
#elif REALSIZE == 8
        switch (bytes) {
        case 4:
            if (swap) {
                for (n = i = 0; n < n_samples; n++, i += spacing) {
                    REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                    REAL_OVERFLOW_UPDATE;
                    sample.r32[0] = (float)realbuf->r64[n];
                    rawbuf->u32[i] = SWAP32(sample.u32[0]);
                }
            } else {
                for (n = i = 0; n < n_samples; n++, i += spacing) {
                    REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                    REAL_OVERFLOW_UPDATE;
                    rawbuf->r32[i] = (float)realbuf->r64[n];
                }
            }
            break;
        case 8:
            if (swap) {
                for (n = i = 0; n < n_samples; n++, i += spacing) {
                    REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                    REAL_OVERFLOW_UPDATE;
                    rawbuf->u64[i] = SWAP64(realbuf->u64[n]);
                }
            } else {
                for (n = i = 0; n < n_samples; n++, i += spacing) {
                    REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                    REAL_OVERFLOW_UPDATE;
                    rawbuf->r64[i] = realbuf->r64[n];
                }
            }
            break;
        default:
            goto real2raw_invalid_byte_size;
        }
#else
 #error invalid REALSIZE
#endif        
	return;
    }
    
    imin = -((uint64_t)1 << (bits - 1));
    imax = ((uint64_t)1 << (bits - 1)) - 1;
    rmin = (REAL_T)imin;
    rmax = (REAL_T)imax;
    switch (bytes) {
    case 1:
	for (n = i = 0; n < n_samples; n++, i += spacing) {
            REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
	    rawbuf->i8[i] = (int8_t)REAL2INT_CALL;
	}
	break;
    case 2:
        if (swap) {
            for (n = i = 0; n < n_samples; n++, i += spacing) {
                REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                sample.i16[0] = (int16_t)REAL2INT_CALL;
                rawbuf->u16[i] = SWAP16(sample.u16[0]);
            }
        } else {
            for (n = i = 0; n < n_samples; n++, i += spacing) {
                REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                rawbuf->i16[i] = (int16_t)REAL2INT_CALL;
            }
        }
	break;
    case 3:
	spacing = spacing * 3 - 3;
#ifdef __BIG_ENDIAN__
        if (swap) {
            for (n = i = 0; n < n_samples; n++, i += spacing) {
                REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                sample.i32[0] = REAL2INT_CALL;
                rawbuf->u8[i++] = sample.u8[3];
                rawbuf->u8[i++] = sample.u8[2];
                rawbuf->u8[i++] = sample.u8[1];
            }
        } else {
            for (n = i = 0; n < n_samples; n++, i += spacing) {
                REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                sample.i32[0] = REAL2INT_CALL;
                rawbuf->u8[i++] = sample.u8[1];
                rawbuf->u8[i++] = sample.u8[2];
                rawbuf->u8[i++] = sample.u8[3];
            }
        }
#endif        
#ifdef __LITTLE_ENDIAN__
        if (swap) {
            for (n = i = 0; n < n_samples; n++, i += spacing) {
                REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                sample.i32[0] = REAL2INT_CALL;
                rawbuf->u8[i++] = sample.u8[2];
                rawbuf->u8[i++] = sample.u8[1];
                rawbuf->u8[i++] = sample.u8[0];
            }
        } else {
            for (n = i = 0; n < n_samples; n++, i += spacing) {
                REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                sample.i32[0] = REAL2INT_CALL;
                rawbuf->u8[i++] = sample.u8[0];
                rawbuf->u8[i++] = sample.u8[1];
                rawbuf->u8[i++] = sample.u8[2];
            }
        }
#endif
	break;
    case 4:	
        if (swap) {
            for (n = i = 0; n < n_samples; n++, i += spacing) {
                REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                sample.i32[0] = REAL2INT_CALL;
                rawbuf->u32[i] = SWAP32(sample.u32[0]);
            }
        } else {
            for (n = i = 0; n < n_samples; n++, i += spacing) {
                REAL2RAW_SAMPLE_TEST(realbuf->RXX[n], overflow);
                rawbuf->i32[i] = REAL2INT_CALL;
            }
        }
	break;
    default:
    real2raw_invalid_byte_size:
	fprintf(stderr, "Sample byte size %d is not supported.\n", bytes);
	bf_exit(BF_EXIT_OTHER);
	break;
    }        
}

#undef REAL_OVERFLOW_UPDATE
#undef RXX
#undef REAL_T
