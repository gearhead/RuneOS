/*
 * (c) Copyright 2001 - 2006, 2013, 2016 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include "defs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/times.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/mman.h>
#ifdef __OS_SUNOS__
#include <ieeefp.h>
#endif

#include "dai.h"
#include "convolver.h"
#include "emalloc.h"
#include "shmalloc.h"
#include "bfrun.h"
#include "fdrw.h"
#include "bit.h"
#include "bfconf.h"
#include "inout.h"
#include "log2.h"
#include "dither.h"
#include "timestamp.h"
#include "delay.h"
#include "pinfo.h"
#include "timermacros.h"

#define DEBUG_MAX_DAI_LOOPS 32
#define DEBUG_RING_BUFFER_SIZE 1024

/* debug structs */
struct debug_input_process {
    struct debug_input d[DEBUG_MAX_DAI_LOOPS];
    int dai_loops;
    struct {
        uint64_t ts_call;
        uint64_t ts_ret;
    } r_output;
    struct {
        uint64_t ts_call;
        uint64_t ts_ret;
    } w_filter;
};

struct debug_output_process {
    struct {
        uint64_t ts_call;
        uint64_t ts_ret;
    } r_filter;
    struct {
        uint64_t ts_call;
        uint64_t ts_ret;
    } w_input;
    struct debug_output d[DEBUG_MAX_DAI_LOOPS];
    int dai_loops;
};

struct debug_filter_process {
    struct {
        uint64_t ts_call;
        uint64_t ts_ret;
    } r_input;
    struct {
        uint64_t ts_call;
        uint64_t ts_ret;
    } mutex;
    struct {
        uint64_t ts_call;
        uint64_t ts_ret;
    } fsynch_fd;
    struct {
        uint64_t ts_call;
        uint64_t ts_ret;
    } fsynch_td;
    struct {
        uint64_t ts_call;
        uint64_t ts_ret;
    } w_output;    
};


struct intercomm_area {
    volatile bool_t doreset_overflow;
    int sync[BF_MAXPROCESSES];
    volatile uint32_t period_us[BF_MAXPROCESSES];
    volatile double realtime_index;
    struct bffilter_control fctrl[BF_MAXFILTERS];
    struct bfoverflow overflow[BF_MAXCHANNELS];
    uint32_t ismuted[2][BF_MAXCHANNELS/32];
    volatile int delay[2][BF_MAXCHANNELS];
    volatile int subdelay[2][BF_MAXCHANNELS];
    volatile int n_pids;
    volatile pid_t pids[BF_MAXPROCESSES];
    volatile int exit_status;
    volatile bool_t full_proc[BF_MAXPROCESSES];
    volatile bool_t ignore_rtprio;

    struct {
        uint64_t ts_start;
        struct debug_input_process i[DEBUG_RING_BUFFER_SIZE];
        struct debug_output_process o[DEBUG_RING_BUFFER_SIZE];
        struct debug_filter_process f[DEBUG_RING_BUFFER_SIZE];
        uint32_t periods;
    } debug;
    
};

static volatile struct intercomm_area *icomm = NULL;
static struct bfoverflow *reset_overflow;
static int bl_output_2_bl_input[2];
static int bl_output_2_cb_input[2];
static int cb_output_2_bl_input[2];
static int cb_input_2_filter[2];
static int filter_2_cb_output[2];
static int mutex_pipe[2];
static int n_callback_devs[2];
static int n_blocking_devs[2];

struct {
    int n_fdpeak;
    int *fdpeak;
    int n_fdinitialised;
    int *fdinitialised;
    int n_peak;
    void (**peak)(void);
    int n_initialised;
    void (**initialised)(void);
    int n_block_start;
    void (**block_start)(struct bfaccess *bfaccess,
                         unsigned int block_index,
                         struct timeval *current_time);
    int n_input_timed;
    void (**input_timed)(void *buf,
			 int channel);
    int n_input_freqd;
    void (**input_freqd)(void *buf,
			 int channel);
    int n_coeff_final;
    void (**coeff_final)(int filter,
                         int *coeff);
    int n_pre_convolve;
    void (**pre_convolve)(void *buf,
			  int filter);
    int n_post_convolve;
    void (**post_convolve)(void *buf,
			   int filter);
    int n_output_freqd;
    void (**output_freqd)(void *buf,
			  int channel);
    int n_output_timed;
    void (**output_timed)(void *buf,
			  int channel);
} events;

#define INIT_EVENTS_FD(id, fdname, counter)                                    \
    if (bfconf->logicmods[n].bfevents.fdevents & id) {                         \
	events. fdname [events. counter ] = bfconf->logicmods[n].event_pipe[1];\
	events. counter ++;                                                    \
    }

#define INIT_EVENTS_FUN(funname, counter)                                      \
    if (bfconf->logicmods[n].bfevents. funname != NULL) {                      \
	events. funname [events. counter] =                                    \
	    bfconf->logicmods[n].bfevents. funname;                            \
	events. counter ++;                                                    \
    }

static void
init_events(void)
{
    int n;
    
    memset(&events, 0, sizeof(events));
    events.fdpeak = emalloc(bfconf->n_logicmods * sizeof(int));
    events.fdinitialised = emalloc(bfconf->n_logicmods * sizeof(int));
    events.peak = emalloc(bfconf->n_logicmods * sizeof(void *));
    events.initialised = emalloc(bfconf->n_logicmods * sizeof(void *));
    events.block_start = emalloc(bfconf->n_logicmods * sizeof(void *));
    events.input_timed = emalloc(bfconf->n_logicmods * sizeof(void *));
    events.input_freqd = emalloc(bfconf->n_logicmods * sizeof(void *));
    events.coeff_final = emalloc(bfconf->n_logicmods * sizeof(void *));
    events.pre_convolve = emalloc(bfconf->n_logicmods * sizeof(void *));
    events.post_convolve = emalloc(bfconf->n_logicmods * sizeof(void *));
    events.output_freqd = emalloc(bfconf->n_logicmods * sizeof(void *));
    events.output_timed = emalloc(bfconf->n_logicmods * sizeof(void *));
    for (n = 0; n < bfconf->n_logicmods; n++) {
	INIT_EVENTS_FD(BF_FDEVENT_PEAK, fdpeak, n_fdpeak);
	INIT_EVENTS_FD(BF_FDEVENT_INITIALISED, fdinitialised, n_fdinitialised);
	INIT_EVENTS_FUN(peak, n_peak);
	INIT_EVENTS_FUN(initialised, n_initialised);
	INIT_EVENTS_FUN(block_start, n_block_start);
	INIT_EVENTS_FUN(input_timed, n_input_timed);
	INIT_EVENTS_FUN(input_freqd, n_input_freqd);
	INIT_EVENTS_FUN(coeff_final, n_coeff_final);
	INIT_EVENTS_FUN(pre_convolve, n_pre_convolve);
	INIT_EVENTS_FUN(post_convolve, n_post_convolve);

	INIT_EVENTS_FUN(output_freqd, n_output_freqd);
	INIT_EVENTS_FUN(output_timed, n_output_timed);
    }
    if (events.n_coeff_final > 1) {
        fprintf(stderr, "It makes no sense to have more than one module "
                "which wants final coefficient control\n");
        bf_exit(BF_EXIT_INVALID_CONFIG);
    }
}

#undef INIT_EVENTS_FD
#undef INIT_EVENTS_FUN

#define D icomm->debug

static void
print_debug(void)
{
    uint64_t tsdiv;
    uint32_t n, i, k;

    printf("\nWARNING: these timestamps only make sense if:\n");
    printf(" - there is only one input device\n");
    printf(" - there is only one output device\n");
    printf(" - there is only one filter process\n");
    printf(" - dai loop does not exceed %d\n\n", DEBUG_MAX_DAI_LOOPS);
    
    tsdiv = (uint64_t)bfconf->cpu_mhz;
    printf("%u periods\n\n", D.periods);

    D.periods += 2;

    if (bfconf->synched_write) {
        printf("output_process:\n");
        for (n = 0; n < 2; n++) {
            printf("  period %i: (dai loop %d)\n", (int)n - 2,
                   D.o[n].dai_loops);
            if (n == 1) {
                printf("    %" PRIu64 "\tcall synch input 0 (write)\n",
                       (ull_t)((D.o[n].w_input.ts_call - D.ts_start) / tsdiv));
                printf("    %" PRIu64 "\tret\n\n",
                       (ull_t)((D.o[n].w_input.ts_ret - D.ts_start) / tsdiv));
            }
            for (i = 0;
                 i < D.o[n].dai_loops && i < DEBUG_MAX_DAI_LOOPS;
                 i++)
            {
                printf("    %" PRIu64 "\tcall select fdmax %d\n",
                       (ull_t)((D.o[n].d[i].select.ts_call -
                                D.ts_start) / tsdiv),
                       D.o[n].d[i].select.fdmax);
                printf("    %" PRIu64 "\tret %d\n",
                       (ull_t)((D.o[n].d[i].select.ts_ret -
                                D.ts_start) / tsdiv),
                       D.o[n].d[i].select.retval);
                printf("    %" PRIu64 "\twrite(%d, %p, %d, %d)\n",
                       (ull_t)((D.o[n].d[i].write.ts_call -
                                D.ts_start) / tsdiv),
                       D.o[n].d[i].write.fd,
                       D.o[n].d[i].write.buf,
                       D.o[n].d[i].write.offset,
                       D.o[n].d[i].write.count);
                printf("    %" PRIu64 "\tret %d\n\n",
                       (ull_t)((D.o[n].d[i].write.ts_ret - D.ts_start) / tsdiv),
                       D.o[n].d[i].write.retval);
            }
            if (n == 0) {
                printf("    %" PRIu64 "\tcall synch input trigger start "
                       "(write)\n",
                       (ull_t)((D.o[n].d[0].init.ts_synchfd_call -
                                D.ts_start) / tsdiv));
                printf("    %" PRIu64 "\tret\n\n",
                       (ull_t)((D.o[n].d[0].init.ts_synchfd_ret -
                                D.ts_start) / tsdiv));
            }
        }
    } else {
        printf("output_process:\n");
        printf("  period -2:\n");
        printf("    %" PRIu64 "\tcall synch input trigger start (write)\n",
               (ull_t)((D.o[0].w_input.ts_call - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tret\n\n",
               (ull_t)((D.o[0].w_input.ts_ret - D.ts_start) / tsdiv));
        printf("output_process:\n");
        printf("  period -1:\n");
        printf("    %" PRIu64 "\tcall synch input 0 (write)\n",
               (ull_t)((D.o[1].w_input.ts_call - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tret\n\n",
               (ull_t)((D.o[1].w_input.ts_ret - D.ts_start) / tsdiv));
    }
    
    for (n = 0;
         n < D.periods && n < DEBUG_RING_BUFFER_SIZE - 2;
         n++)
    {
        printf("input_process:\n");
        printf("  period %u: (dai loop %d)\n", n, D.i[n].dai_loops);
        if (n == 0) {
            printf("    %" PRIu64 "\tcall init start\n",
                   (ull_t)((D.i[n].d[0].init.ts_start_call -
                            D.ts_start) / tsdiv));
            printf("    %" PRIu64 "\tret\n",
                   (ull_t)((D.i[n].d[0].init.ts_start_ret -
                            D.ts_start) / tsdiv));
        }
        for (i = 0;
             i < D.i[n].dai_loops && i < DEBUG_MAX_DAI_LOOPS;
             i++)
        {
            printf("    %" PRIu64 "\tcall select fdmax %d\n",
                   (ull_t)((D.i[n].d[i].select.ts_call - D.ts_start) / tsdiv),
                   D.i[n].d[i].select.fdmax);
            printf("    %" PRIu64 "\tret %d (%" PRIu64 ")\n",
                   (ull_t)((D.i[n].d[i].select.ts_ret - D.ts_start) / tsdiv),
                   D.i[n].d[i].select.retval,
                   (ull_t)((D.i[n].d[i].select.ts_ret - D.ts_start) / tsdiv -
                           (D.i[n].d[i].select.ts_call - D.ts_start) / tsdiv));
            printf("    %" PRIu64 "\tread(%d, %p, %d, %d)\n",
                   (ull_t)((D.i[n].d[i].read.ts_call - D.ts_start) / tsdiv),
                   D.i[n].d[i].read.fd,
                   D.i[n].d[i].read.buf,
                   D.i[n].d[i].read.offset,
                   D.i[n].d[i].read.count);
            printf("    %" PRIu64 "\tret %d\n\n",
                   (ull_t)((D.i[n].d[i].read.ts_ret - D.ts_start) / tsdiv),
                   D.i[n].d[i].read.retval);
        }
        printf("    %" PRIu64 "\tcall synch output %d (read)\n",
               (ull_t)((D.i[n].r_output.ts_call - D.ts_start) / tsdiv), n - 1);
        printf("    %" PRIu64 "\tret\n",
               (ull_t)((D.i[n].r_output.ts_ret - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tcall synch filter %d (write)\n",
               (ull_t)((D.i[n].w_filter.ts_call - D.ts_start) / tsdiv), n);
        printf("    %" PRIu64 "\tret\n\n",
               (ull_t)((D.i[n].w_filter.ts_ret - D.ts_start) / tsdiv));


        printf("filter_process:\n");
        printf("  period %u:\n", n);
        printf("    %" PRIu64 "\tcall synch input %d (read)\n",
               (ull_t)((D.f[n].r_input.ts_call - D.ts_start) / tsdiv), n);
        printf("    %" PRIu64 "\tret\n",
               (ull_t)((D.f[n].r_input.ts_ret - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tcall mutex\n",
               (ull_t)((D.f[n].mutex.ts_call - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tret\n\n",
               (ull_t)((D.f[n].mutex.ts_ret - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tcall synch fd\n",
               (ull_t)((D.f[n].fsynch_fd.ts_call - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tret\n\n",
               (ull_t)((D.f[n].fsynch_fd.ts_ret - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tcall synch td\n",
               (ull_t)((D.f[n].fsynch_td.ts_call - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tret\n\n",
               (ull_t)((D.f[n].fsynch_td.ts_ret - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tcall synch output %d (write)\n",
               (ull_t)((D.f[n].w_output.ts_call - D.ts_start) / tsdiv), n);
        printf("    %" PRIu64 "\tret (%" PRIu64 " from synch input ret)\n\n",
               (ull_t)((D.f[n].w_output.ts_ret - D.ts_start) / tsdiv),
               (ull_t)((D.f[n].w_output.ts_ret - D.ts_start) / tsdiv -
                       (D.f[n].r_input.ts_ret - D.ts_start) / tsdiv));


        n += 2;
        printf("output_process:\n");
        printf("  period %u: (dai loop %d)\n", n - 2,
               D.o[n].dai_loops);
        printf("    %" PRIu64 "\tcall synch filter %d (read)\n",
               (ull_t)((D.o[n].r_filter.ts_call - D.ts_start) / tsdiv), n - 2);
        printf("    %" PRIu64 "\tret (%" PRId64 ")\n",
               (ull_t)((D.o[n].r_filter.ts_ret - D.ts_start) / tsdiv),
               (ll_t)((D.o[n].r_filter.ts_ret - D.ts_start) / tsdiv -
                      (D.o[n].r_filter.ts_call - D.ts_start) / tsdiv));
        printf("    %" PRIu64 "\tcall synch input %d (write)\n",
               (ull_t)((D.o[n].w_input.ts_call - D.ts_start) / tsdiv), n - 1);
        printf("    %" PRIu64 "\tret (%" PRId64 ")\n\n",
               (ull_t)((D.o[n].w_input.ts_ret - D.ts_start) / tsdiv),
               (ll_t)((D.o[n].w_input.ts_ret - D.ts_start) / tsdiv -
                      (D.i[n-2].r_output.ts_ret - D.ts_start) / tsdiv));
        if (!bfconf->synched_write && n == 2) {
            printf("    %" PRIu64 "\tcall init start\n", 
                   (ull_t)((D.o[n].d[0].init.ts_start_call -
                            D.ts_start) / tsdiv));
            printf("    %" PRIu64 "\tret\n", 
                   (ull_t)((D.o[n].d[0].init.ts_start_ret -
                            D.ts_start) / tsdiv));
        }
        for (i = 0;
             i < D.o[n].dai_loops && i < DEBUG_MAX_DAI_LOOPS;
             i++)
        {
            printf("    %" PRIu64 "\tcall select fdmax %d\n",
                   (ull_t)((D.o[n].d[i].select.ts_call - D.ts_start) / tsdiv),
                   D.o[n].d[i].select.fdmax);
            printf("    %" PRIu64 "\tret %d\n",
                   (ull_t)((D.o[n].d[i].select.ts_ret - D.ts_start) / tsdiv),
                   D.o[n].d[i].select.retval);
            if (D.o[n-1].dai_loops > DEBUG_MAX_DAI_LOOPS) {
                k = DEBUG_MAX_DAI_LOOPS - 1;
            } else {
                k = D.o[n-1].dai_loops - 1;
            }
            printf("    %" PRIu64 "\twrite(%d, %p, %d, %d) (%" PRIu64 ")\n",
                   (ull_t)((D.o[n].d[i].write.ts_call - D.ts_start) / tsdiv),
                   D.o[n].d[i].write.fd,
                   D.o[n].d[i].write.buf,
                   D.o[n].d[i].write.offset,
                   D.o[n].d[i].write.count,
                   (ull_t)((D.o[n].d[i].write.ts_call - D.ts_start) / tsdiv -
                           (D.o[n-1].d[k-1].write.ts_call -
                            D.ts_start) / tsdiv));
            printf("    %" PRIu64 "\tret %d\n\n",
                   (ull_t)((D.o[n].d[i].write.ts_ret - D.ts_start) / tsdiv),
                   D.o[n].d[i].write.retval);
        }
        n -= 2;
        
    }

}

#undef D

static void
sighandler(int sig)
{
    bf_exit(icomm->exit_status);
}

static int
ismuted(int io,
	int channel)
{
    if ((io != IN && io != OUT) ||
	channel < 0 || channel >= bfconf->n_channels[io])
    {
	return (int)true;
    }
    return (int)bit_isset_volatile(icomm->ismuted[io], channel);
}

static void
toggle_mute(int io,
	    int channel)
{
    int physch;
    
    if ((io != IN && io != OUT) ||
	channel < 0 || channel >= bfconf->n_channels[io])
    {
	return;
    }
    if (bit_isset_volatile(icomm->ismuted[io], channel)) {
        bit_clr_volatile(icomm->ismuted[io], channel);
    } else {
        bit_set_volatile(icomm->ismuted[io], channel);
    }
    
    physch = bfconf->virt2phys[io][channel];
    if (bfconf->n_virtperphys[io][physch] == 1) {
	dai_toggle_mute(io, physch);
	return;
    }
}

static int
set_delay(int io,
	  int channel,
	  int delay)
{
    int physch;
    
    if ((io != IN && io != OUT) ||
	channel < 0 || channel >= bfconf->n_channels[io])
    {
	return -1;
    }
    if (delay == icomm->delay[io][channel]) {
        return 0;
    }
    if (delay < 0 || delay > bfconf->maxdelay[io][channel]) {
	return -1;
    }
    physch = bfconf->virt2phys[io][channel];
    if (bfconf->n_virtperphys[io][physch] == 1) {
	if (dai_change_delay(io, physch, delay) == -1) {
            return -1;
        }
    }
    icomm->delay[io][channel] = delay;
    return 0;
}

static int
get_delay(int io,
	  int channel)
{
    if ((io != IN && io != OUT) ||
	channel < 0 || channel >= bfconf->n_channels[OUT])
    {
	return 0;
    }
    return icomm->delay[io][channel];
}

static int
set_subdelay(int io,
             int channel,
             int subdelay)
{
    if ((io != IN && io != OUT) ||
	channel < 0 || channel >= bfconf->n_channels[io] ||
        subdelay <= -BF_SAMPLE_SLOTS || subdelay >= BF_SAMPLE_SLOTS)
    {
	return -1;
    }
    if (subdelay == icomm->subdelay[io][channel]) {
        return 0;
    }
    if (!bfconf->use_subdelay[io] ||
        bfconf->subdelay[io][channel] == BF_UNDEFINED_SUBDELAY)
    {
        return -1;
    }
    icomm->subdelay[io][channel] = subdelay;
    return 0;
}

static int
get_subdelay(int io,
             int channel)
{
    if ((io != IN && io != OUT) ||
	channel < 0 || channel >= bfconf->n_channels[OUT])
    {
	return 0;
    }
    return icomm->subdelay[io][channel];
}

static void
print_overflows(void)
{
    bool_t is_overflow = false;
    double peak;
    int n;
    
    for (n = 0; n < bfconf->n_channels[OUT]; n++) {
	if (icomm->overflow[n].n_overflows > 0) {
	    is_overflow = true;
	    break;
	}
    }
    if (!is_overflow && !bfconf->show_progress) {
	return;
    }
    pinfo("peak: ");
    for (n = 0; n < bfconf->n_channels[OUT]; n++) {
	peak = icomm->overflow[n].largest;
        if (peak < (double)icomm->overflow[n].intlargest) {
            peak = (double)icomm->overflow[n].intlargest;
        }
	if (peak != 0.0) {
	    if ((peak = 20.0 * log10(peak / icomm->overflow[n].max)) == 0.0) {
		peak = -0.0; /* we want to display -0.0 rather than +0.0 */
	    }
            pinfo("%d/%u/%+.2f ", n, icomm->overflow[n].n_overflows, peak);
	} else {
            pinfo("%d/%u/-Inf ", n, icomm->overflow[n].n_overflows);
        }
    }
    pinfo("\n");    
}

static void
check_overflows(struct bfoverflow overflow[])
{
    struct bfoverflow of;
    uint32_t msg;
    int n;
    
    if (!bfconf->overflow_warnings) {
        return;
    }
    for (n = 0; n < bfconf->n_channels[OUT]; n++) {
        of = icomm->overflow[n];
        if (memcmp(&of, &overflow[n], sizeof(struct bfoverflow)) != 0) {
            for (; n < bfconf->n_channels[OUT]; n++) {
                overflow[n] = icomm->overflow[n];
            }
            msg = BF_FDEVENT_PEAK;
            for (n = 0; n < events.n_fdpeak; n++) {
                if (!writefd(events.fdpeak[n], &msg, 4)) {
                    bf_exit(BF_EXIT_OTHER);
                }
            }
            for (n = 0; n < events.n_peak; n++) {
                events.peak[n]();
            }
            print_overflows();
            return;
        }
    }
}

static void
rti_and_overflow(void)
{
    static struct bfoverflow overflow[BF_MAXCHANNELS];
    static time_t lastprinttime = 0;
    static uint32_t max_period_us;
    static bool_t isinit = false;
    
    double rti, max_rti;
    uint32_t period_us;
    bool_t full_proc;
    time_t tt;
    int n;

    if (!isinit) {
        for (n = 0; n < bfconf->n_channels[OUT]; n++) {
            overflow[n] = icomm->overflow[n];
        }
        max_period_us = (uint32_t)((uint64_t)bfconf->filter_length * 1000000 /
                                   bfconf->sampling_rate);
        isinit = true;
    }
    
    if (icomm->doreset_overflow) {
        icomm->doreset_overflow = false;
        memset(overflow, 0, sizeof(struct bfoverflow) *
               bfconf->n_channels[OUT]);
    }

    /* calculate realtime index */
    if ((tt = time(NULL)) != lastprinttime) {
        max_rti = 0;
        full_proc = true;
        for (n = 0; n < bfconf->n_processes; n++) {
            period_us = icomm->period_us[n];
            if (!icomm->full_proc[n]) {
                full_proc = false;
            }
            if (period_us == 0) {
                max_rti = 0;
                break;
            }                
            rti = (float)period_us / (float)max_period_us;
            if (rti > max_rti) {
                max_rti = rti;
            }
        }
        if (bfconf->show_progress && max_rti != 0) {
            if (full_proc) {
                pinfo("rti: %.3f\n", max_rti);
            } else {
                pinfo("rti: not full processing - no rti update\n");
            }
        }
        icomm->realtime_index = max_rti;
        check_overflows(overflow);
        lastprinttime = tt;
    }
}

static void
icomm_mutex(int lock)
{
    char dummydata[1];
    
    dummydata[0] = '\0';
    if (lock) {
        if (!readfd(mutex_pipe[0], dummydata, 1)) {
            bf_exit(BF_EXIT_OTHER);
        }
    } else {
        if (!writefd(mutex_pipe[1], dummydata, 1)) {
            bf_exit(BF_EXIT_OTHER);
        }
    }
}

static bool_t
memiszero(void *buf,
          int size)
{
    int n, count;
    uint32_t acc;

    /* we go through all memory always, to avoid variations in time it takes */
    
    acc = 0;
    count = size >> 2;
    for (n = 0; n < count; n += 4) {
        acc |= ((uint32_t *)buf)[n+0];
        acc |= ((uint32_t *)buf)[n+1];
        acc |= ((uint32_t *)buf)[n+2];
        acc |= ((uint32_t *)buf)[n+3];
    }
    count = size - (count << 2);
    buf = &((uint32_t *)buf)[n];
    for (n = 0; n < count; n++) {
        acc |= ((uint8_t *)buf)[n];
    }
    return acc == 0;
}

static bool_t
test_silent(void *buf,
            int size,
            int realsize,
            double analog_powersave,
            double scale)
{
    int n, count;
    double dmax;
    float fmax;
    
    if (analog_powersave >= 1.0) {
        return memiszero(buf, size);
    }
    if (realsize == 4) {
        fmax = 0;
        count = size >> 2;
        for (n = 0; n < count; n++) {
            if (((float *)buf)[n] < 0) {
                if (-((float *)buf)[n] > fmax) {
                    fmax = -((float *)buf)[n];
                }
            } else {
                if (((float *)buf)[n] > fmax) {
                    fmax = ((float *)buf)[n];
                }
            }
        }
        dmax = fmax;
    } else {
        dmax = 0;
        count = size >> 3;
        for (n = 0; n < count; n++) {
            if (((double *)buf)[n] < 0) {
                if (-((double *)buf)[n] > dmax) {
                    dmax = -((double *)buf)[n];
                }
            } else {
                if (((double *)buf)[n] > dmax) {
                    dmax = ((double *)buf)[n];
                }
            }
        }
    }
    if (scale * dmax >= analog_powersave) {
        return false;
    }
    /* make it truly zero */
    memset(buf, 0, size);
    return true;
}

static void
input_process(void *buf[2],
	      int filter_writefd,
	      int output_readfd,
              int extra_output_readfd,
	      int synch_writefd)
{
    char dummydata[bfconf->n_processes];
    bool_t do_yield;
    int n, curbuf;
    uint32_t msg;
    int dbg_pos;
    
    if (bfconf->realtime_priority) {
	bf_make_realtime(0, bfconf->realtime_midprio, "input");
    }

    do_yield = false;
    if (dai_minblocksize() >= bfconf->filter_length || dai_input_poll_mode()) {
        do_yield = true;
    }
    
    msg = BF_FDEVENT_INITIALISED;
    for (n = 0; n < events.n_fdinitialised; n++) {
	if (!writefd(events.fdinitialised[n], &msg, 4)) {
            bf_exit(BF_EXIT_OTHER);
        }
    }
    for (n = 0; n < events.n_initialised; n++) {
	events.initialised[n]();
    }
    
    dbg_pos = 0;
    curbuf = 0;
    memset(dummydata, 0, bfconf->n_processes);
    
    if (synch_writefd != -1) {
        if (!writefd(synch_writefd, dummydata, 1) ||
            !readfd(output_readfd, dummydata, 1))
        {
            bf_exit(BF_EXIT_OTHER);
        }
    }

    while (true) {
	dai_input(icomm->debug.i[dbg_pos].d, DEBUG_MAX_DAI_LOOPS,
                  &icomm->debug.i[dbg_pos].dai_loops);
	curbuf = !curbuf;

        timestamp(&icomm->debug.i[dbg_pos].r_output.ts_call);        
	if (!readfd(output_readfd, dummydata, 1) ||
            (extra_output_readfd != -1 &&
             !readfd(extra_output_readfd, dummydata, 1)))
        {
            bf_exit(BF_EXIT_OTHER);
        }
        timestamp(&icomm->debug.i[dbg_pos].r_output.ts_ret);

        timestamp(&icomm->debug.i[dbg_pos].w_filter.ts_call);
        if (!writefd(filter_writefd, dummydata, bfconf->n_processes)) {
            bf_exit(BF_EXIT_OTHER);
        }
        if (bfconf->realtime_priority && do_yield) {
            sched_yield();
        }
        timestamp(&icomm->debug.i[dbg_pos].w_filter.ts_ret);
        if (++dbg_pos == DEBUG_RING_BUFFER_SIZE) {
            dbg_pos = 0;
        }
    }
}

static void
output_process(int filter_readfd,
	       int synch_readfd,
	       int input_writefd,
               int extra_input_writefd,
               bool_t trigger_callback_io,
               bool_t checkdrift)
{
    char dummydata[bfconf->n_processes];
    uint32_t bufindex = 0;
    int dbg_pos;

    if (bfconf->realtime_priority) {
        bf_make_realtime(0, bfconf->realtime_midprio, "output");
    }

    dbg_pos = 0;
    memset(dummydata, 0, bfconf->n_processes);
    if (synch_readfd != -1) {
        if (!readfd(synch_readfd, dummydata, 1)) {
            bf_exit(BF_EXIT_OTHER);
        }
    }
    /* verify if we need to write iodelay output */
    if (bfconf->synched_write) {
        pinfo("Fixed I/O-delay is %d samples\n"
              "Audio processing starts now\n", 2 * bfconf->filter_length +
            (bfconf->use_subdelay[IN] ? bfconf->sdf_length : 0) +
            (bfconf->use_subdelay[OUT] ? bfconf->sdf_length : 0));
        if (trigger_callback_io) {
            dai_trigger_callback_io();
        }
	if (extra_input_writefd != -1 &&
            !writefd(extra_input_writefd, dummydata, 1))
        {
            bf_exit(BF_EXIT_OTHER);
        }
	dai_output(true, input_writefd,
                   icomm->debug.o[dbg_pos].d,
                   DEBUG_MAX_DAI_LOOPS,
                   &icomm->debug.o[dbg_pos].dai_loops);
        dbg_pos++;
        timestamp(&icomm->debug.o[dbg_pos].w_input.ts_call);
	if (!writefd(input_writefd, dummydata, 1) ||
            (extra_input_writefd != -1 &&
             !writefd(extra_input_writefd, dummydata, 1)))
        {
            bf_exit(BF_EXIT_OTHER);
        }
        timestamp(&icomm->debug.o[dbg_pos].w_input.ts_ret);
	dai_output(true, -1,
                   icomm->debug.o[dbg_pos].d,
                   DEBUG_MAX_DAI_LOOPS,
                   &icomm->debug.o[dbg_pos].dai_loops);
        dbg_pos++;
    } else {
        pinfo("Audio processing starts now\n");
        if (trigger_callback_io) {
            dai_trigger_callback_io();
        }
        timestamp(&icomm->debug.o[dbg_pos].w_input.ts_call);
	if (!writefd(input_writefd, dummydata, 1) ||
            (extra_input_writefd != -1 &&
             !writefd(extra_input_writefd, dummydata, 1)))
        {
            bf_exit(BF_EXIT_OTHER);
        }
        timestamp(&icomm->debug.o[dbg_pos].w_input.ts_ret);
        dbg_pos++;
        timestamp(&icomm->debug.o[dbg_pos].w_input.ts_call);
	if (!writefd(input_writefd, dummydata, 1) ||
            (extra_input_writefd != -1 &&
             !writefd(extra_input_writefd, dummydata, 1)))
        {
            bf_exit(BF_EXIT_OTHER);
        }
        timestamp(&icomm->debug.o[dbg_pos].w_input.ts_ret);
        dbg_pos++;
    }
    icomm->debug.periods = 0;

    while (true) {
        timestamp(&icomm->debug.o[dbg_pos].r_filter.ts_call);
        if (!readfd(filter_readfd, dummydata, bfconf->n_processes)) {
            bf_exit(BF_EXIT_OTHER);
	}
        timestamp(&icomm->debug.o[dbg_pos].r_filter.ts_ret);
        timestamp(&icomm->debug.o[dbg_pos].w_input.ts_call);
	if (!writefd(input_writefd, dummydata, 1) ||
            (extra_input_writefd != -1 &&
             !writefd(extra_input_writefd, dummydata, 1)))
        {
            bf_exit(BF_EXIT_OTHER);
        }
        timestamp(&icomm->debug.o[dbg_pos].w_input.ts_ret);

	/* write output */
	dai_output(false, -1,
                   icomm->debug.o[dbg_pos].d,
                   DEBUG_MAX_DAI_LOOPS,
                   &icomm->debug.o[dbg_pos].dai_loops);

        rti_and_overflow();
        
	bufindex++;

        if (++dbg_pos == DEBUG_RING_BUFFER_SIZE) {
            dbg_pos = 0;
        }
        icomm->debug.periods++;

        if (bfconf->debug &&
            icomm->debug.periods == DEBUG_RING_BUFFER_SIZE - 4)
        {
            fprintf(stderr, "Debug timestamp buffer is now full, exiting\n");
            bf_exit(BF_EXIT_OTHER);
        }
    }
}

struct apply_subdelay_params {
    int subdelay;
    void *rest;    
};

static void
apply_subdelay(void *realbuf,
               int n_samples,
               void *arg)
{
    struct apply_subdelay_params *p;

    p = (struct apply_subdelay_params *)arg;

    if (p->rest == NULL) {
        return;
    }
    delay_subsample_update(realbuf, p->rest, p->subdelay);
}

static void
synch_filter_processes(int filter_readfd,
                       int filter_writefd[],
                       int process_index)
{
    int n;
    char dummydata[bfconf->n_processes - 1];
    if (bfconf->n_processes > 1) {
        for (n = 0; n < bfconf->n_processes; n++) {
            if (n != process_index) {
                dummydata[0] = 0;
                if (!writefd(filter_writefd[n], dummydata, 1)) {
                    bf_exit(BF_EXIT_OTHER);
                }
            }
        }
        if (!readfd(filter_readfd, dummydata, bfconf->n_processes - 1)) {
            bf_exit(BF_EXIT_OTHER);
        }
    }
}

static void
filter_process(struct bfaccess *bfaccess,
               void *inbuf[2],
	       void *outbuf[2],
	       void *input_freqcbuf[],
	       void *output_freqcbuf[],
	       int filter_readfd,
	       int filter_writefd[],
	       int input_readfd,
               int cb_input_readfd,
	       int output_writefd,
               int cb_output_writefd,
	       int n_procinputs,
	       int procinputs[],
	       int n_procoutputs,
	       int procoutputs[],
	       int n_inputs,
	       int inputs[],
	       int n_outputs,
	       int outputs[],
	       int n_filters,
	       struct bffilter filters[],
	       int process_index,
               bool_t has_bl_input_devs,
               bool_t has_bl_output_devs,
               bool_t has_cb_input_devs,
               bool_t has_cb_output_devs)
{
    int convbufsize  = convolver_cbufsize();
    int fragsize = bfconf->filter_length;
    int n_blocks = bfconf->n_blocks;
    int curblock = 0;
    int curbuf = 0;
    
    void *input_timecbuf[n_procinputs][2];
    void **mixconvbuf_inputs[n_filters];
    void **mixconvbuf_filters[n_filters];
    void *cbuf[n_filters][n_blocks];
    void *ocbuf[n_filters];
    void *evalbuf[n_filters];
    void *static_evalbuf = NULL;
    void *inbuf_copy = NULL;
    
    double *outscale[BF_MAXCHANNELS][n_filters];
    double scales[n_filters + BF_MAXCHANNELS];
    double virtscales[2][BF_MAXCHANNELS];
    void *crossfadebuf[2];
    void *mixbuf = NULL;
    void *outconvbuf[BF_MAXCHANNELS][n_filters];
    int outconvbuf_n_filters[BF_MAXCHANNELS];
    unsigned int blockcounter = 0;
    delaybuffer_t *output_db[BF_MAXCHANNELS];
    delaybuffer_t *input_db[BF_MAXCHANNELS];
    void *output_sd_rest[BF_MAXCHANNELS];
    void *input_sd_rest[BF_MAXCHANNELS];
    bool_t need_crossfadebuf = false;
    bool_t need_mixbuf = false;
    bool_t mixbuf_is_filled;
    int inbuf_copy_size;
  
    int n, i, j, coeff, delay, cblocks, prevcblocks, physch, virtch;
    struct buffer_format *bf, inbuf_copy_bf;
    uint8_t *memptr, *baseptr;
    struct bfoverflow of;
    uint32_t dummydata32;
    char dummydata[1];

    int memsize, icomm_delay[2][BF_MAXCHANNELS];
    struct bffilter_control icomm_fctrl[n_filters];
    uint32_t icomm_ismuted[2][BF_MAXCHANNELS/32];
    bool_t powersave, change_prio, first_print;
    int icomm_subdelay[2][BF_MAXCHANNELS];
    struct apply_subdelay_params sd_params;
    int dbg_pos, subdelay_fb_size;

    int prevcoeff[n_filters];
    int procblocks[n_filters];
    uint32_t partial_proc[n_filters / 32 + 1];
    int *mixconvbuf_filters_map[n_filters];
    int outconvbuf_map[BF_MAXCHANNELS][n_filters];
    bool_t input_freqcbuf_zero[bfconf->n_channels[IN]];
    bool_t output_freqcbuf_zero[bfconf->n_channels[OUT]];
    bool_t cbuf_zero[n_filters][n_blocks];
    bool_t ocbuf_zero[n_filters];
    bool_t evalbuf_zero[n_filters];
    bool_t temp_buffer_zero;
    bool_t iszero;    
    
    struct timeval period_start, period_end, tv;
    int32_t period_length;
    double clockmul;
    uint64_t t1, t2, t3, t4;
    uint64_t t[10];
    uint32_t cc = 0;

    dummydata[0] = '\0';
    dbg_pos = 0;
    first_print = true;
    change_prio = false;
    powersave = bfconf->powersave;
    if (dai_minblocksize() == 0 || dai_minblocksize() < bfconf->filter_length) {
        change_prio = true;
    }

    subdelay_fb_size = delay_subsample_filterblocksize();
    temp_buffer_zero = false;
    memset(procblocks, 0, n_filters * sizeof(int));
    memset(partial_proc, 0xFF, (n_filters / 32 + 1) * sizeof(uint32_t));
    memset(evalbuf_zero, 0, n_filters * sizeof(bool_t));
    memset(ocbuf_zero, 0, n_filters * sizeof(bool_t));
    memset(cbuf_zero, 0, n_blocks * n_filters * sizeof(bool_t));
    memset(output_freqcbuf_zero, 0, bfconf->n_channels[OUT] * sizeof(bool_t));
    memset(input_freqcbuf_zero, 0, bfconf->n_channels[IN] * sizeof(bool_t));
    memset(crossfadebuf, 0, sizeof(crossfadebuf));
    memset(icomm_subdelay, 0, sizeof(icomm_subdelay));

    if (!readfd(input_readfd, dummydata, 1)) { /* for init */
        bf_exit(BF_EXIT_OTHER);
    }
    synch_filter_processes(filter_readfd, filter_writefd, process_index);

    /* allocate input delay buffers */
    for (n = j = 0; n < n_procinputs; n++) {
	virtch = procinputs[n];
	physch = bfconf->virt2phys[IN][virtch];
        if (bfconf->use_subdelay[IN] &&
            bfconf->subdelay[IN][virtch] != BF_UNDEFINED_SUBDELAY)
        {
            input_sd_rest[virtch] =
                emallocaligned(subdelay_fb_size * bfconf->realsize);
            memset(input_sd_rest[virtch], 0,
                   subdelay_fb_size * bfconf->realsize);
        } else {
            input_sd_rest[virtch] = NULL;
        }
	if (bfconf->n_virtperphys[IN][physch] > 1) {
	    for (i = 0; i < bfconf->n_subdevs[IN]; i++) {
		if (bfconf->subdevs[IN][i].channels.channel_name
		    [bfconf->subdevs[IN][i].channels.used_channels-1] >=
		    physch)
		{
		    break;
		}
	    }
            delay = 0;
            if (bfconf->use_subdelay[IN] &&
                bfconf->subdelay[IN][virtch] == BF_UNDEFINED_SUBDELAY)
            {
                delay = bfconf->sdf_length;
            }
	    input_db[virtch] =
		delay_allocate_buffer(fragsize,
				      icomm->delay[IN][virtch] + delay,
				      bfconf->maxdelay[IN][virtch] + delay,
				      bfconf->subdevs[IN][i].channels.sf.bytes);
	    if (bfconf->subdevs[IN][i].channels.sf.bytes > j) {
		j = bfconf->subdevs[IN][i].channels.sf.bytes;
	    }
	} else {
	    /* delays on channels with direct 1-1 virtual-physical mapping are
	       taken care of in the dai module instead */
	    input_db[virtch] = NULL;
	}
    }
    inbuf_copy_size = j * fragsize;
    inbuf_copy_bf.sample_spacing = 1;
    inbuf_copy_bf.byte_offset = 0;
    
    /* allocate output delay buffers */
    for (n = 0; n < n_procoutputs; n++) {
	virtch = procoutputs[n];
	physch = bfconf->virt2phys[OUT][virtch];
        if (bfconf->use_subdelay[OUT] &&
            bfconf->subdelay[OUT][virtch] != BF_UNDEFINED_SUBDELAY)
        {
            output_sd_rest[virtch] =
                emallocaligned(subdelay_fb_size * bfconf->realsize);
            memset(output_sd_rest[virtch], 0,
                   subdelay_fb_size * bfconf->realsize);
        } else {
            output_sd_rest[virtch] = NULL;
        }
	if (bfconf->n_virtperphys[OUT][physch] > 1) {
            delay = 0;
            if (bfconf->use_subdelay[OUT] &&
                bfconf->subdelay[OUT][virtch] == BF_UNDEFINED_SUBDELAY)
            {
                delay = bfconf->sdf_length;
            }
	    output_db[virtch] =
		delay_allocate_buffer(fragsize,
				      icomm->delay[OUT][virtch] + delay,
				      bfconf->maxdelay[OUT][virtch] + delay,
				      bfconf->realsize);
	    need_mixbuf = true;
	} else {
	    output_db[virtch] = NULL;
	}
    }
    
    /* find out if there is a need of evaluation buffers, and how many,
       and if there is a need for a crossfade buffer */
    for (n = i = j = 0; n < n_filters; n++) {
	if (filters[n].n_filters[IN] > 0) {
	    i++;
	}
        if (filters[n].crossfade) {
            need_crossfadebuf = true;
        }
    }

    /* allocate input/output/evaluation convolve buffers */
    if (inbuf_copy_size > convbufsize) {
	/* this should never happen, since convbufsize should be
	   2 * fragsize * realsize, sample sizes should never exceed
	   8 bytes, and realsize never be smaller than 4 bytes */
	fprintf(stderr, "Unexpected buffer sizes.\n");
	bf_exit(BF_EXIT_OTHER);
    }
    if (n_blocks > 1) {
	memsize = n_filters * n_blocks * convbufsize +
	    n_filters * convbufsize +
	    i * (convbufsize + convbufsize / 2) +
	    2 * n_procinputs * convbufsize;
    } else {
	memsize = n_filters * convbufsize +
	    i * (convbufsize + convbufsize / 2) +
	    2 * n_procinputs * convbufsize;
    }
    if (i > 0) {
        memsize += convbufsize;
        if (need_crossfadebuf) {
            memsize += convbufsize;
        }
    } else {
        if (need_crossfadebuf) {
            memsize += 2 * convbufsize;
        } else if (need_mixbuf) {
            memsize += fragsize * bfconf->realsize;
        }
    }
    memptr = emallocaligned(memsize);
    baseptr = memptr;
    if (i > 0) {
        if (need_crossfadebuf) {
            crossfadebuf[0] = memptr + memsize - 2 * convbufsize;
            crossfadebuf[1] = memptr + memsize - convbufsize;
            static_evalbuf = crossfadebuf[0];
        } else {
            static_evalbuf = memptr + memsize - convbufsize;
        }
        if (need_mixbuf) {
            mixbuf = static_evalbuf;
        }
    } else {
        if (need_crossfadebuf) {
            crossfadebuf[0] = memptr + memsize - 2 * convbufsize;
            crossfadebuf[1] = memptr + memsize - convbufsize;
            if (need_mixbuf) {
                mixbuf = crossfadebuf[0];
            }
        } else if (need_mixbuf) {
            mixbuf = memptr + memsize - fragsize * bfconf->realsize;
        }
    }
    if (n_blocks > 1) {
        for (n = 0; n < n_filters; n++) {
	    for (i = 0; i < n_blocks; i++) {
		cbuf[n][i] = memptr;
		memptr += convbufsize;
	    }
	    if (filters[n].n_filters[IN] > 0) {
		evalbuf[n] = memptr;
		memptr += (convbufsize + convbufsize / 2);
	    } else {
		evalbuf[n] = NULL;
	    }
	    ocbuf[n] = memptr;
	    memptr += convbufsize;
	}
    } else {
	for (n = 0; n < n_filters; n++) {
	    cbuf[n][0] = ocbuf[n] = memptr;
	    memptr += convbufsize;
	    if (filters[n].n_filters[IN] > 0) {
		evalbuf[n] = memptr;
		memptr += (convbufsize + convbufsize / 2);
	    } else {
		evalbuf[n] = NULL;
	    }
	}
    }
    inbuf_copy = ocbuf[0];
    for (n = 0; n < n_procinputs; n++, memptr += 2 * convbufsize) {
	input_timecbuf[n][0] = memptr;
	input_timecbuf[n][1] = memptr + convbufsize;
    }
    /* for each filter, find out which channel-inputs that are mixed */
    for (n = 0; n < n_filters; n++) {
	if (filters[n].n_filters[IN] > 0) {
	    /* allocate extra position for filter-input evaluation buffer */
	    mixconvbuf_inputs[n] =
		alloca((filters[n].n_channels[IN] + 1) * sizeof(void **));
	    mixconvbuf_inputs[n][filters[n].n_channels[IN]] = NULL;
	} else if (filters[n].n_channels[IN] == 0) {
	    mixconvbuf_inputs[n] = NULL;
	    continue;
	} else {
	    mixconvbuf_inputs[n] =
		alloca(filters[n].n_channels[IN] * sizeof(void **));
	}
	for (i = 0; i < filters[n].n_channels[IN]; i++) {
	    mixconvbuf_inputs[n][i] =
		input_freqcbuf[filters[n].channels[IN][i]];
	}
    }
    /* for each filter, find out which filter-inputs that are mixed */
    for (n = 0; n < n_filters; n++) {
        prevcoeff[n] = icomm->fctrl[filters[n].intname].coeff;
	if (filters[n].n_filters[IN] == 0) {
	    mixconvbuf_filters[n] = NULL;
	    mixconvbuf_filters_map[n] = NULL;
	    continue;
	}
	mixconvbuf_filters[n] =
	    alloca(filters[n].n_filters[IN] * sizeof(void **));
	mixconvbuf_filters_map[n] =
	    alloca(filters[n].n_filters[IN] * sizeof(int));
	for (i = 0; i < filters[n].n_filters[IN]; i++) {
            /* find out index of filter */
            for (j = 0; j < n_filters; j++) {
                if (filters[n].filters[IN][i] == filters[j].intname) {
                    break;
                }
            }
            mixconvbuf_filters_map[n][i] = j;
	    mixconvbuf_filters[n][i] = ocbuf[j];
	}
    }
   
    /* for each unique output channel, find out which filters that
       mixes its output to it */
    memset(outconvbuf_n_filters, 0, sizeof(outconvbuf_n_filters));
    for (n = 0; n < n_outputs; n++) {
	for (i = 0; i < n_filters; i++) {
	    for (j = 0; j < filters[i].n_channels[OUT]; j++) {
		if (filters[i].channels[OUT][j] == outputs[n]) {
                    outconvbuf_map[n][outconvbuf_n_filters[n]] = i;
                    outconvbuf[n][outconvbuf_n_filters[n]] = ocbuf[i];
		    outscale[n][outconvbuf_n_filters[n]] =
			       &icomm_fctrl[i].scale[OUT][j];
		    outconvbuf_n_filters[n]++;
		    /* output exists only once per filter, we can break here */
		    break;
		}
	    }
	}
    }

    /* calculate scales */
    FOR_IN_AND_OUT {
	for (n = 0; n < bfconf->n_channels[IO]; n++) {
	    physch = bfconf->virt2phys[IO][n];
	    virtscales[IO][n] = dai_buffer_format[IO]->bf[physch].sf.scale;
	}	
    }

    if (bfconf->debug) {
	fprintf(stderr, "(%d) got %d inputs, %d outputs\n", (int)getpid(),
                n_procinputs, n_procoutputs);
	for (n = 0; n < n_procinputs; n++) {
	    fprintf(stderr, "(%d) input: %d\n", (int)getpid(), procinputs[n]);
	}
	for (n = 0; n < n_procoutputs; n++) {
	    fprintf(stderr, "(%d) output: %d\n", (int)getpid(), procoutputs[n]);
	}
    }

    /* access all memory while being nobody, so we don't risk getting killed
       later if memory is scarce */
    memset(baseptr, 0, memsize);    
    for (n = 0; n < bfconf->n_coeffs; n++) {
        for (i = 0; i < bfconf->coeffs[n].n_blocks; i++) {
            memcpy(ocbuf[0], bfconf->coeffs_data[n][i], convbufsize);
        }
    }
    dummydata32 = 0;
    for (n = 0; n < sizeof(struct intercomm_area) / sizeof(uint32_t); n++) {
        dummydata32 += ((volatile uint32_t *)icomm)[n];
    }
    memset(ocbuf[0], 0, convbufsize);
    memset(inbuf[0], 0, dai_buffer_format[IN]->n_bytes);
    memset(inbuf[1], 0, dai_buffer_format[IN]->n_bytes);
    memset(outbuf[0], 0, dai_buffer_format[OUT]->n_bytes);
    memset(outbuf[1], 0, dai_buffer_format[OUT]->n_bytes);
    for (n = 0; n < n_inputs; n++) {
        memset(input_freqcbuf[inputs[n]], 0, convbufsize);
    }
    for (n = 0; n < n_outputs; n++) {
        memset(output_freqcbuf[outputs[n]], 0, convbufsize);
    }
    
    if (bfconf->realtime_priority) {
        /* priority is lowered later if necessary */
        bf_make_realtime(0, bfconf->realtime_maxprio, "filter");
    }
    if (!writefd(output_writefd, dummydata, 1)) { /* for init */
        bf_exit(BF_EXIT_OTHER);
    }
    
    /* main filter loop starts here */
    memset(t, 0, sizeof(t));
    while (true) {
        gettimeofday(&period_end, NULL);

	/* wait for next input buffer */
        timestamp(&icomm->debug.f[dbg_pos].r_input.ts_call);
        if (has_bl_input_devs) {
            if (!readfd(input_readfd, dummydata, 1)) {
                bf_exit(BF_EXIT_OTHER);
            }
        }
        if (has_cb_input_devs) {
            if (!readfd(cb_input_readfd, dummydata, 1)) {
                bf_exit(BF_EXIT_OTHER);
            }
        }
        timestamp(&icomm->debug.f[dbg_pos].r_input.ts_ret);
        /* we only calculate period length if all filters are processing
           full length */
        if (bit_find(partial_proc, 0, n_filters - 1) == -1) {
            timersub(&period_end, &period_start, &tv);
            period_length = tv.tv_sec * 1000000 + tv.tv_usec;
            icomm->period_us[process_index] = period_length;
            icomm->full_proc[process_index] = true;
        } else {
            icomm->full_proc[process_index] = false;
        }
        gettimeofday(&period_start, NULL);

        if (events.n_block_start > 0) {
            if (process_index == 0) {
                for (i = 0; i < events.n_block_start; i++) {
                    tv = period_start;
                    events.block_start[i](bfaccess, blockcounter, &tv);
                }
            }
            synch_filter_processes(filter_readfd, filter_writefd,
                                   process_index);
        }
        
        /* get all shared memory data we need where mutex is important */
        timestamp(&icomm->debug.f[dbg_pos].mutex.ts_call);
        icomm_mutex(1);
        for (n = 0; n < n_filters; n++) {
            icomm_fctrl[n].coeff = icomm->fctrl[filters[n].intname].coeff;
            icomm_fctrl[n].delayblocks =
                icomm->fctrl[filters[n].intname].delayblocks;
            for (i = 0; i < filters[n].n_channels[IN]; i++) {
                icomm_fctrl[n].scale[IN][i] =
                    icomm->fctrl[filters[n].intname].scale[IN][i];
            }
            for (i = 0; i < filters[n].n_channels[OUT]; i++) {
                icomm_fctrl[n].scale[OUT][i] =
                    icomm->fctrl[filters[n].intname].scale[OUT][i];
            }
            for (i = 0; i < filters[n].n_filters[IN]; i++) {
                icomm_fctrl[n].fscale[i] =
                    icomm->fctrl[filters[n].intname].fscale[i];
            }
        }
        memcpy(icomm_ismuted, (void *)icomm->ismuted, sizeof(icomm_ismuted));
        memcpy(icomm_delay, (void *)icomm->delay, sizeof(icomm_delay));
        if (bfconf->use_subdelay[IN] || bfconf->use_subdelay[OUT]) {
            memcpy(icomm_subdelay, (void *)icomm->subdelay, sizeof(icomm_subdelay));
        }
        icomm_mutex(0);

        /* change to lower priority so we can be pre-empted, but we only do so
           if required by the input (or output) process */
        if (bfconf->realtime_priority && change_prio) {
            bf_make_realtime(0, bfconf->realtime_minprio, NULL);
        }
        timestamp(&icomm->debug.f[dbg_pos].mutex.ts_ret);
        
	timestamp(&t3);
	for (n = 0; n < n_procinputs; n++) {
	    /* convert inputs */
	    timestamp(&t1);
	    virtch = procinputs[n];
	    physch = bfconf->virt2phys[IN][virtch];
	    bf = &dai_buffer_format[IN]->bf[physch];
            sd_params.subdelay = icomm_subdelay[IN][virtch];
            sd_params.rest = input_sd_rest[virtch];
	    if (bfconf->n_virtperphys[IN][physch] == 1) {
                convolver_raw2cbuf(inbuf[curbuf],
                                   input_timecbuf[n][curbuf],
                                   input_timecbuf[n][!curbuf],
                                   bf,
                                   apply_subdelay,
                                   (void *)&sd_params);
	    } else {
		if (!bit_isset(icomm_ismuted[IN], virtch)) {
                    delay = icomm_delay[IN][virtch];
                    if (bfconf->use_subdelay[IN] &&
                        bfconf->subdelay[IN][virtch] == BF_UNDEFINED_SUBDELAY)
                    {
                        delay += bfconf->sdf_length;
                    }
		    delay_update(input_db[virtch],
				 &((uint8_t *)inbuf[curbuf])[bf->byte_offset],
				 bf->sf.bytes, bf->sample_spacing,
				 delay,
				 inbuf_copy);
		} else {
		    memset(inbuf_copy, 0, fragsize * bf->sf.bytes);
		}
                inbuf_copy_bf.sf = bf->sf;
                convolver_raw2cbuf(inbuf_copy,
                                   input_timecbuf[n][curbuf],
                                   input_timecbuf[n][!curbuf],
                                   &inbuf_copy_bf,
                                   apply_subdelay,
                                   (void *)&sd_params);
	    }
	    for (i = 0; i < events.n_input_timed; i++) {
		events.input_timed[i](input_timecbuf[n][curbuf], procinputs[n]);
	    }
	    timestamp(&t2);
	    t[0] += t2 - t1;
	    
	    /* transform to frequency domain */
	    timestamp(&t1);
            if (!powersave ||
                !test_silent(input_timecbuf[n][curbuf], convbufsize,
                             bfconf->realsize,
                             bfconf->analog_powersave,
                             bf->sf.scale))
            {
                convolver_time2freq(input_timecbuf[n][curbuf],
                                    input_freqcbuf[procinputs[n]]);
                input_freqcbuf_zero[procinputs[n]] = false;
            } else if (!input_freqcbuf_zero[procinputs[n]]) {
                memset(input_freqcbuf[procinputs[n]], 0, convbufsize);
                input_freqcbuf_zero[procinputs[n]] = true;
            }
	    for (i = 0; i < events.n_input_freqd; i++) {
		events.input_freqd[i](input_freqcbuf[procinputs[n]],
				      procinputs[n]);
	    }
	    timestamp(&t2);
	    t[1] += t2 - t1;
	}

        timestamp(&icomm->debug.f[dbg_pos].fsynch_fd.ts_call);
        synch_filter_processes(filter_readfd, filter_writefd, process_index);
        timestamp(&icomm->debug.f[dbg_pos].fsynch_fd.ts_ret);

	for (n = 0; n < n_filters; n++) {
            if (procblocks[n] < n_blocks) {
                procblocks[n]++;
            } else {
                bit_clr(partial_proc, n);
            }
	    timestamp(&t1);
            coeff = icomm_fctrl[n].coeff;
	    if (events.n_coeff_final == 1) {
                /* this module wants final control of the choice of
                   coefficient */
		events.coeff_final[0](filters[n].intname, &coeff);
	    }
	    delay = icomm_fctrl[n].delayblocks;
	    if (delay < 0) {
		delay = 0;
	    } else if (delay > n_blocks - 1) {
		delay = n_blocks - 1;
	    }
	    if (coeff < 0 ||
		bfconf->coeffs[coeff].n_blocks > n_blocks - delay)
	    {
		cblocks = n_blocks - delay;
	    } else {
		cblocks = bfconf->coeffs[coeff].n_blocks;
	    }
	    if (prevcoeff[n] < 0 ||
		bfconf->coeffs[prevcoeff[n]].n_blocks > n_blocks - delay)
	    {
		prevcblocks = n_blocks - delay;
	    } else {
		prevcblocks = bfconf->coeffs[prevcoeff[n]].n_blocks;
	    }

	    curblock = (int)((blockcounter + delay) % (unsigned int)(n_blocks));
	    
	    /* mix and scale inputs prior to convolution */
	    if (filters[n].n_filters[IN] > 0) {
		/* mix, scale and reorder filter-inputs for evaluation in the
		   time domain. */
                iszero = true;
                for (i = 0; i < filters[n].n_filters[IN]; i++) {
                    if (!ocbuf_zero[mixconvbuf_filters_map[n][i]]) {
                        iszero = false;
                        break;
                    }
                }
                if (!iszero || !powersave) {
                    convolver_mixnscale(mixconvbuf_filters[n],
                                        static_evalbuf,
                                        icomm_fctrl[n].fscale,
                                        filters[n].n_filters[IN],
                                        CONVOLVER_MIXMODE_OUTPUT);
                    temp_buffer_zero = false;
                } else if (!temp_buffer_zero) {
                    memset(static_evalbuf, 0, convbufsize);
                    temp_buffer_zero = true;
                }
                
		/* evaluate convolution */
                if (!temp_buffer_zero || !evalbuf_zero[n] || !powersave) {
                    convolver_convolve_eval(static_evalbuf,
                                            evalbuf[n],
                                            static_evalbuf);
                    evalbuf_zero[n] = false;
                    if (temp_buffer_zero) {
                        evalbuf_zero[n] = true;
                        temp_buffer_zero = false;
                    }
                }
		
		/* mix and scale channel-inputs and reorder prior to
		   convolution */
                iszero = temp_buffer_zero;
		for (i = 0; i < filters[n].n_channels[IN]; i++) {
		    scales[i] = icomm_fctrl[n].scale[IN][i] *
			virtscales[IN][filters[n].channels[IN][i]];
                    if (!input_freqcbuf_zero[filters[n].channels[IN][i]]) {
                        iszero = false;
                    }
		}
		/* FIXME: unecessary scale multiply for filter-inputs */
		scales[i] = 1.0;
		mixconvbuf_inputs[n][i] = static_evalbuf;
                if (!iszero || !powersave) {
                    convolver_mixnscale(mixconvbuf_inputs[n],
                                        cbuf[n][curblock],
                                        scales,
                                        filters[n].n_channels[IN] + 1,
                                        CONVOLVER_MIXMODE_INPUT);
                    cbuf_zero[n][curblock] = false;
                } else if (!cbuf_zero[n][curblock]) {
                    memset(cbuf[n][curblock], 0, convbufsize);
                    cbuf_zero[n][curblock] = true;
                }
	    } else {
                iszero = true;
		for (i = 0; i < filters[n].n_channels[IN]; i++) {
		    scales[i] = icomm_fctrl[n].scale[IN][i] *
			virtscales[IN][filters[n].channels[IN][i]];
                    if (!input_freqcbuf_zero[filters[n].channels[IN][i]]) {
                        iszero = false;
                    }
		}
                if (!iszero || !powersave) {
                    convolver_mixnscale(mixconvbuf_inputs[n],
                                        cbuf[n][curblock],
                                        scales,
                                        filters[n].n_channels[IN],
                                        CONVOLVER_MIXMODE_INPUT);
                    cbuf_zero[n][curblock] = false;
                } else if (!cbuf_zero[n][curblock]) {
                    memset(cbuf[n][curblock], 0, convbufsize);
                    cbuf_zero[n][curblock] = true;
                }
	    }
	    timestamp(&t2);
	    t[2] += t2 - t1;
	    /* convolve (or not) */
	    timestamp(&t1);

	    curblock = (int)(blockcounter % (unsigned int)n_blocks);
	    for (i = 0; i < events.n_pre_convolve; i++) {
		events.pre_convolve[i](cbuf[n][curblock], n);
	    }
	    if (coeff >= 0) {
		if (n_blocks == 1) {
                    /* curblock is always zero when n_blocks == 1 */
                    if (!cbuf_zero[n][0] || !powersave) {
                        if (filters[n].crossfade && prevcoeff[n] != coeff) {
                            if (prevcoeff[n] < 0) {
                                convolver_dirac_convolve(cbuf[n][0],
                                                         crossfadebuf[0]);
                            } else {
                                convolver_convolve
                                    (cbuf[n][0],
                                     bfconf->coeffs_data[prevcoeff[n]][0],
                                     crossfadebuf[0]);
                            }
                            convolver_convolve_inplace
                                (cbuf[n][0],
                                 bfconf->coeffs_data[coeff][0]);
                            convolver_crossfade_inplace(cbuf[n][0],
                                                        crossfadebuf[0],
                                                        crossfadebuf[1]);
                            temp_buffer_zero = false;
                        } else {
                            convolver_convolve_inplace
                                (cbuf[n][0],
                                 bfconf->coeffs_data[coeff][0]);
                        }
                        /* cbuf points at ocbuf when n_blocks == 1 */
                        ocbuf_zero[n] = false;
                    } else {
                        ocbuf_zero[n] = true;
                        procblocks[n] = 0;
                        bit_set(partial_proc, n);
                    }
		} else {
                    if (!cbuf_zero[n][curblock] || !powersave) {
                        if (filters[n].crossfade && prevcoeff[n] != coeff) {
                            if (prevcoeff[n] < 0) {
                                convolver_dirac_convolve(cbuf[n][curblock],
                                                         crossfadebuf[0]);
                            } else {
                                convolver_convolve
                                    (cbuf[n][curblock],
                                     bfconf->coeffs_data[prevcoeff[n]][0],
                                     crossfadebuf[0]);
                            }
                        }
                        convolver_convolve(cbuf[n][curblock],
                                           bfconf->coeffs_data[coeff][0],
                                           ocbuf[n]);
                        ocbuf_zero[n] = false;
                    } else if (!ocbuf_zero[n]) {
                        memset(ocbuf[n], 0, convbufsize);
                        ocbuf_zero[n] = true;
                    }
		    for (i = 1; i < cblocks && i < procblocks[n]; i++) {
			j = (int)((blockcounter - i) % (unsigned int)n_blocks);
                        if (!cbuf_zero[n][j] || !powersave) {
                            convolver_convolve_add
                                (cbuf[n][j],
                                 bfconf->coeffs_data[coeff][i],
                                 ocbuf[n]);
                            ocbuf_zero[n] = false;
                        }
		    }
                    if (filters[n].crossfade && prevcoeff[n] != coeff &&
                        prevcoeff[n] >= 0)
                    {
                        for (i = 1; i < prevcblocks && i < procblocks[n]; i++) {
                            j = (int)((blockcounter - i) %
                                      (unsigned int)n_blocks);
                            if (!cbuf_zero[n][j] || !powersave) {
                                convolver_convolve_add
                                    (cbuf[n][j],
                                     bfconf->coeffs_data[prevcoeff[n]][i],
                                     crossfadebuf[0]);
                            }
                            ocbuf_zero[n] = false;
                        }
		    }
                    if (ocbuf_zero[n]) {
                        procblocks[n] = 0;
                        bit_set(partial_proc, n);
                    } else if (filters[n].crossfade && prevcoeff[n] != coeff) {
                        convolver_crossfade_inplace(ocbuf[n], crossfadebuf[0],
                                                    crossfadebuf[1]);
                        temp_buffer_zero = false;
                    }
		}
	    } else {
		if (n_blocks == 1) {
                    if (!cbuf_zero[n][0] || !powersave) {
                        if (filters[n].crossfade && prevcoeff[n] != coeff) {
                            convolver_convolve
                                (cbuf[n][0],
                                 bfconf->coeffs_data[prevcoeff[n]][0],
                                 crossfadebuf[0]);
                            convolver_dirac_convolve_inplace(cbuf[n][0]);
                            convolver_crossfade_inplace(cbuf[n][0],
                                                        crossfadebuf[0],
                                                        crossfadebuf[1]);
                            temp_buffer_zero = false;
                        } else {
                            convolver_dirac_convolve_inplace(cbuf[n][0]);
                        }
                        ocbuf_zero[n] = false;
                    } else {
                        ocbuf_zero[n] = true;
                        procblocks[n] = 0;
                        bit_set(partial_proc, n);
                    }
		} else {
                    if (!cbuf_zero[n][curblock] || !powersave) {
                        if (filters[n].crossfade && prevcoeff[n] != coeff) {
                            convolver_convolve
                                (cbuf[n][curblock],
                                 bfconf->coeffs_data[prevcoeff[n]][0],
                                 crossfadebuf[0]);
                        }
                        convolver_dirac_convolve(cbuf[n][curblock], ocbuf[n]);
                        ocbuf_zero[n] = false;
                    } else if (!ocbuf_zero[n]) {
                        memset(ocbuf[n], 0, convbufsize);
                        ocbuf_zero[n] = true;
                    }
                    if (filters[n].crossfade && prevcoeff[n] != coeff) {
                        for (i = 1; i < prevcblocks && i < procblocks[n]; i++) {
                            j = (int)((blockcounter - i) %
                                      (unsigned int)n_blocks);
                            if (!cbuf_zero[n][j] || !powersave) {
                                convolver_convolve_add
                                    (cbuf[n][j],
                                     bfconf->coeffs_data[prevcoeff[n]][i],
                                     crossfadebuf[0]);
                            }
                            ocbuf_zero[n] = false;
                        }
                    }
                    if (ocbuf_zero[n]) {
                        procblocks[n] = 0;
                        bit_set(partial_proc, n);
                    } else if (filters[n].crossfade && prevcoeff[n] != coeff) {
                        convolver_crossfade_inplace(ocbuf[n], crossfadebuf[0],
                                                    crossfadebuf[1]);
                        temp_buffer_zero = false;
                    }
		}
	    }
            prevcoeff[n] = coeff;
	    for (i = 0; i < events.n_post_convolve; i++) {
		events.post_convolve[i](cbuf[n][curblock], n);
	    }
	    timestamp(&t2);
	    t[3] += t2 - t1;
	}
	
	timestamp(&t1);
	for (n = 0; n < n_outputs; n++) {
            iszero = true;
	    for (i = 0; i < outconvbuf_n_filters[n]; i++) {
		scales[i] = *outscale[n][i] / virtscales[OUT][outputs[n]];
                if (!ocbuf_zero[outconvbuf_map[n][i]]) {
                    iszero = false;
                }
	    }
	    /* mix and scale convolve outputs prior to conversion to time
	       domain */
            if (!iszero || !powersave) {
                convolver_mixnscale(outconvbuf[n],
                                    output_freqcbuf[outputs[n]],
                                    scales,
                                    outconvbuf_n_filters[n],
                                    CONVOLVER_MIXMODE_OUTPUT);
                output_freqcbuf_zero[outputs[n]] = false;
            } else if (!output_freqcbuf_zero[outputs[n]]) {
                memset(output_freqcbuf[outputs[n]], 0, convbufsize);
                output_freqcbuf_zero[outputs[n]] = true;
            }
	}
	timestamp(&t2);
	t[4] += t2 - t1;
	
        timestamp(&icomm->debug.f[dbg_pos].fsynch_td.ts_call);
        synch_filter_processes(filter_readfd, filter_writefd, process_index);
        timestamp(&icomm->debug.f[dbg_pos].fsynch_td.ts_ret);

	mixbuf_is_filled = false;
	for (n = j = 0; n < n_procoutputs; n++) {	    
	    /* transform back to time domain */
	    timestamp(&t1);
	    virtch = procoutputs[n];
	    physch = bfconf->virt2phys[OUT][virtch];
	    for (i = 0; i < events.n_output_freqd; i++) {
		events.output_freqd[i](output_freqcbuf[virtch], virtch);
	    }
	    /* ocbuf[0] happens to be free, that's why we use it */
            if (!output_freqcbuf_zero[virtch] || !powersave) {
                convolver_freq2time(output_freqcbuf[virtch], ocbuf[0]);
                ocbuf_zero[0] = false;
                if (n_blocks == 1) {
                    cbuf_zero[0][0] = false;
                }
            } else if (!ocbuf_zero[0]) {
                memset(ocbuf[0], 0, convbufsize);
                ocbuf_zero[0] = true;
                if (n_blocks == 1) {
                    cbuf_zero[0][0] = true;
                }
            }

            /* Check if there is NaN or Inf values, and abort if so. We cannot
               afford to check all values, but NaN/Inf tend to spread, so
               checking only one value usually catches the problem. */
            if ((bfconf->realsize == sizeof(float) &&
                 !finite((double)((float *)ocbuf[0])[0])) ||
                (bfconf->realsize == sizeof(double) &&
                 !finite(((double *)ocbuf[0])[0])))
            {
                fprintf(stderr, "NaN or Inf values in the system! "
                        "Invalid input? Aborting.\n");
                bf_exit(BF_EXIT_OTHER);
            }
            
	    timestamp(&t2);
	    t[5] += t2 - t1;
	    
	    /* write to output buffer */
	    timestamp(&t1);
	    for (i = 0; i < events.n_output_timed; i++) {
		events.output_timed[i](ocbuf[0], virtch);
	    }
            if (output_sd_rest[virtch] != NULL) {
                delay_subsample_update(ocbuf[0],
                                       output_sd_rest[virtch],
                                       icomm_subdelay[OUT][virtch]);
            }
	    if (bfconf->n_virtperphys[OUT][physch] == 1) {
		/* only one virtual channel allocated to this physical one, so
		   we write to it directly */                
                of = icomm->overflow[virtch];
                convolver_cbuf2raw(ocbuf[0],
                                   outbuf[curbuf],
                                   &dai_buffer_format[OUT]->bf[physch],
                                   bfconf->dither_state[physch] != NULL,
                                   bfconf->dither_state[physch],
                                   &of);
                icomm->overflow[virtch] = of;
            } else {
		/* Mute, delay and mix. This is done in the dai module normally,
		   where we get lower I/O-delay on mute and delay operations.
		   However, when mixing to a single physical channel we cannot
		   do it there, so we must do it here instead. */
                delay = icomm_delay[OUT][virtch];
                if (bfconf->use_subdelay[OUT] &&
                    bfconf->subdelay[OUT][virtch] == BF_UNDEFINED_SUBDELAY)
                {
                    delay += bfconf->sdf_length;
                }
		delay_update(output_db[virtch], ocbuf[0], bfconf->realsize, 1,
			     delay, NULL);
		if (!bit_isset(icomm_ismuted[OUT], virtch)) {
		    if (!mixbuf_is_filled) {
			memcpy(mixbuf, ocbuf[0], fragsize * bfconf->realsize);
		    } else {
                        if (bfconf->realsize == 4) {
                            for (i = 0; i < fragsize; i += 4) {
                                ((float *)mixbuf)[i+0] +=
                                    ((float *)ocbuf[0])[i+0];
                                ((float *)mixbuf)[i+1] +=
                                    ((float *)ocbuf[0])[i+1];
                                ((float *)mixbuf)[i+2] +=
                                    ((float *)ocbuf[0])[i+2];
                                ((float *)mixbuf)[i+3] +=
                                    ((float *)ocbuf[0])[i+3];
                            }
                        } else {
                            for (i = 0; i < fragsize; i += 4) {
                                ((double *)mixbuf)[i+0] +=
                                    ((double *)ocbuf[0])[i+0];
                                ((double *)mixbuf)[i+1] +=
                                    ((double *)ocbuf[0])[i+1];
                                ((double *)mixbuf)[i+2] +=
                                    ((double *)ocbuf[0])[i+2];
                                ((double *)mixbuf)[i+3] +=
                                    ((double *)ocbuf[0])[i+3];
                            }
                        }
		    }
                    temp_buffer_zero = false;
		    mixbuf_is_filled = true;
		}
		if (++j == bfconf->n_virtperphys[OUT][physch]) {
		    if (!mixbuf_is_filled) {
                        /* we cannot set temp_buffer_zero here since
                           fragsize * bfconf->realsize is smaller than
                           convbufsize */
			memset(mixbuf, 0, fragsize * bfconf->realsize);
		    }
		    j = 0;
		    mixbuf_is_filled = false;
		    /* overflow structs are same for all virtual channels
		       assigned to a single physical one, so we copy them */
		    of = icomm->overflow[virtch];
		    convolver_cbuf2raw(mixbuf,
				       outbuf[curbuf],
				       &dai_buffer_format[OUT]->bf[physch],
				       bfconf->dither_state[physch] != NULL,
				       bfconf->dither_state[physch],
				       &of);
		    for (i = 0; i < bfconf->n_virtperphys[OUT][physch]; i++) {
			icomm->overflow[bfconf->phys2virt[OUT][physch][i]] = of;
		    }
		}
	    }
	    timestamp(&t2);
	    t[6] += t2 - t1;
	}
	timestamp(&t4);
        t[7] += t4 - t3;

	/* signal the output process */
        timestamp(&icomm->debug.f[dbg_pos].w_output.ts_call);
        if (bfconf->realtime_priority && change_prio) {
            bf_make_realtime(0, bfconf->realtime_maxprio, NULL);
        }
        if (has_bl_output_devs) {
            if (!writefd(output_writefd, dummydata, 1)) {
                bf_exit(BF_EXIT_OTHER);
            }
        }
        if (has_cb_output_devs) {
            if (!writefd(cb_output_writefd, dummydata, 1)) {
                bf_exit(BF_EXIT_OTHER);
            }
        }
        if (bfconf->realtime_priority) {
            sched_yield();
        }
        timestamp(&icomm->debug.f[dbg_pos].w_output.ts_ret);
	
	/* swap convolve buffers */
	curbuf = !curbuf;

	/* advance input block */
	blockcounter++;
	if (bfconf->debug || bfconf->benchmark) {
	    if (++cc % 10 == 0) {
                if (process_index == 0 && first_print) {
                    first_print = false;
                    fprintf(stderr, "\n\
  pid ......... process id of filter process\n\
  raw2real .... sample format conversion from input to internal format\n\
  time2freq ... forward fast fourier transform of input buffers\n\
  mixscale1 ... mixing and scaling (volume) of filter input buffers\n\
  convolve .... convolution of filter buffers (and crossfade if used)\n\
  mixscale2 ... mixing and scaling of filter output buffers\n\
  freq2time ... inverse fast fouirer transform of input buffers\n\
  real2raw .... sample format conversion from internal format to output\n\
  total ....... total time required per period\n\
  periods ..... number of periods processed so far\n\
  rti ......... current realtime index\n\
\n\
all times are in milliseconds, mean value over 10 periods\n\
\n\
  pid |  raw2real | time2freq | mixscale1 |  convolve | mixscale2 | \
freq2time |  real2raw |     total | periods | rti \n\
--------------------------------------------------------------------\
----------------------------------------------------\n");
                }
                clockmul = 1.0 / (bfconf->cpu_mhz * 1000.0);
                for (n = 0; n < 8; n++) {
                    t[n] /= 10;
                }
		fprintf(stderr, "%5d | %9.3f | %9.3f | %9.3f | %9.3f |"
                        " %9.3f | %9.3f | %9.3f | %9.3f | %7lu | %.3f\n",
			(int)getpid(),
			(double)t[0] * clockmul,
			(double)t[1] * clockmul,
			(double)t[2] * clockmul,
			(double)t[3] * clockmul,
			(double)t[4] * clockmul,
			(double)t[5] * clockmul,
			(double)t[6] * clockmul,
			(double)t[7] * clockmul,
                        (unsigned long int)cc,
                        icomm->realtime_index);
                memset(t, 0, sizeof(t));
	    }
	}

        if (++dbg_pos == DEBUG_RING_BUFFER_SIZE) {
            dbg_pos = 0;
        }
    }
}

void
bf_callback_ready(int io)
{
    static bool_t isinit = false;
    char data[bfconf->n_processes];
    
    if (!isinit) {
        if (n_blocking_devs[IN] > 0) {
            /* trigger blocking I/O input, this is done in the first call which
               is for input if there is callback I/O input */
            if (!writefd(cb_output_2_bl_input[1], data, 1)) {
                bf_exit(BF_EXIT_OTHER);
            }
        }
    }
    isinit = true;
    
    memset(data, 0, bfconf->n_processes);
    if (io == IN) {
        if (n_blocking_devs[OUT] > 0) {
            /* wait for blocking I/O output */
            if (!readfd(bl_output_2_cb_input[0], data, 1)) {
                bf_exit(BF_EXIT_OTHER);
            }
        }
        /* trigger filter process(es). Other end will read for each dev */
        if (!writefd(cb_input_2_filter[1], data, bfconf->n_processes)) {
            bf_exit(BF_EXIT_OTHER);
        }
    } else {
        /* wait for filter process(es) */
        if (!readfd(filter_2_cb_output[0], data, bfconf->n_processes)) {
            bf_exit(BF_EXIT_OTHER);
        }
        if (n_blocking_devs[IN] > 0) {
            /* trigger input */
            if (!writefd(cb_output_2_bl_input[1], data, 1)) {
                bf_exit(BF_EXIT_OTHER);
            }
        }
        if (n_blocking_devs[OUT] == 0) {
            rti_and_overflow();
        }
    }
}

void
bfrun(void)
{
    int synch_pipe[2];
    int bl_input_2_filter[2];
    int filter_2_bl_output[2], filter2filter_pipes[bfconf->n_processes][2];
    int filter_writefd[bfconf->n_processes];
    char dummydata[bfconf->n_processes];
    void *buffers[2][2];
    void *input_freqcbuf[bfconf->n_channels[IN]], *input_freqcbuf_base;
    void *output_freqcbuf[bfconf->n_channels[OUT]], *output_freqcbuf_base;
    int nc[2], cpos[2], channels[2][BF_MAXCHANNELS];
    int n, i, j, cbufsize, physch;
    bool_t checkdrift, trigger;
    struct bfaccess bfaccess;
    pid_t pid;

    /* FIXME: check so that unused pipe file descriptors are closed */
    
    memset(dummydata, 0, bfconf->n_processes);
    
    n_callback_devs[IN] = n_callback_devs[OUT] = 0;
    n_blocking_devs[IN] = n_blocking_devs[OUT] = 0;
    FOR_IN_AND_OUT {
        for (n = 0; n < bfconf->n_subdevs[IO]; n++) {
            if (bfconf->iomods[bfconf->subdevs[IO][n].module].iscallback) {
                n_callback_devs[IO]++;
            } else {
                n_blocking_devs[IO]++;
            }
        }
    }
    
    /* allocate shared memory for I/O buffers and interprocess communication */
    cbufsize = convolver_cbufsize();
    if ((input_freqcbuf_base = shmalloc(bfconf->n_channels[IN] * cbufsize))
	== NULL ||
	(output_freqcbuf_base = shmalloc(bfconf->n_channels[OUT] * cbufsize))
	== NULL ||
	(icomm = shmalloc(sizeof(struct intercomm_area))) == NULL)
    {
	fprintf(stderr, "Failed to allocate shared memory: %s.\n",
		strerror(errno));
        bf_exit(BF_EXIT_NO_MEMORY);
        return;
    }
    for (n = 0; n < bfconf->n_channels[IN]; n++) {
	input_freqcbuf[n] = input_freqcbuf_base;
        input_freqcbuf_base = (uint8_t *)input_freqcbuf_base + cbufsize;
    }
    for (n = 0; n < bfconf->n_channels[OUT]; n++) {
	output_freqcbuf[n] = output_freqcbuf_base;
	output_freqcbuf_base = (uint8_t *)output_freqcbuf_base + cbufsize;
    }
    
    /* initialise process intercomm area */
    for (n = 0; n < sizeof(struct intercomm_area); n++) {
        ((volatile uint8_t *)icomm)[n] = 0;
    }
    for (n = 0; n < sizeof(icomm->debug); n++) {
        ((volatile uint8_t *)&icomm->debug)[n] = ~0;
    }
    for (n = 0; n < bfconf->n_filters; n++) {
        icomm->fctrl[n] = bfconf->initfctrl[n];
    }
    icomm->pids[0] = getpid();
    icomm->n_pids = 1;
    icomm->exit_status = BF_EXIT_OK;
    FOR_IN_AND_OUT {
        for (n = 0; n < bfconf->n_channels[IO]; n++) {
            icomm->delay[IO][n] = bfconf->delay[IO][n];
            icomm->subdelay[IO][n] = bfconf->subdelay[IO][n];
            if (bfconf->mute[IO][n]) {
                bit_set_volatile(icomm->ismuted[IO], n);
            } else {
                bit_clr_volatile(icomm->ismuted[IO], n);
            }
        }
    }

    /* install signal handlers */
    if (signal(SIGINT, sighandler) == SIG_ERR ||
	signal(SIGTERM, sighandler) == SIG_ERR)
    {
	fprintf(stderr, "Failed to install signal handlers.\n");
        bf_exit(BF_EXIT_OTHER);
        return;
    }
    
    /* create synchronisation pipes */
    for (n = 0; n < bfconf->n_processes; n++) {
	if (pipe(filter2filter_pipes[n]) == -1) {
	    fprintf(stderr, "Failed to create pipe: %s.\n", strerror(errno));
            bf_exit(BF_EXIT_OTHER);
            return;
	}	
    }
    if (pipe(bl_output_2_bl_input) == -1 ||
        pipe(bl_output_2_cb_input) == -1 ||
        pipe(cb_output_2_bl_input) == -1 ||
        pipe(bl_input_2_filter) == -1 ||
        pipe(filter_2_bl_output) == -1 ||
        pipe(cb_input_2_filter) == -1 ||
        pipe(filter_2_cb_output) == -1 ||
        pipe(mutex_pipe) == -1)
    {
	fprintf(stderr, "Failed to create pipe: %s.\n", strerror(errno));
        bf_exit(BF_EXIT_OTHER);
        return;
    }
    if (!writefd(mutex_pipe[1], dummydata, 1)) {        
        bf_exit(BF_EXIT_OTHER);
        return;
    }
    
    /* init digital audio interfaces for input and output */
    if (!dai_init(bfconf->filter_length, bfconf->sampling_rate,
		  bfconf->n_subdevs, bfconf->subdevs, buffers))
    {
	fprintf(stderr, "Failed to initialise digital audio interfaces.\n");
        bf_exit(BF_EXIT_OTHER);
        return;
    }
    if (dai_buffer_format[IN]->n_samples != bfconf->filter_length ||
	dai_buffer_format[OUT]->n_samples != bfconf->filter_length)
    {
	/* a bug if it happens */
	fprintf(stderr, "Fragment size mismatch.\n");
        bf_exit(BF_EXIT_OTHER);
        return;
    }

    /* init overflow structure */
    reset_overflow = emalloc(sizeof(struct bfoverflow) *
			     bfconf->n_channels[OUT]);
    memset(reset_overflow, 0, sizeof(struct bfoverflow) *
	   bfconf->n_channels[OUT]);
    for (n = 0; n < bfconf->n_channels[OUT]; n++) {
	physch = bfconf->virt2phys[OUT][n];
	if (dai_buffer_format[OUT]->bf[physch].sf.isfloat) {
	    reset_overflow[n].max = 1.0;
	} else {
	    reset_overflow[n].max = (double)
		((uint64_t)1 << ((dai_buffer_format[OUT]->bf[physch].
			sf.sbytes << 3) - 1)) - 1;
	}
        icomm->overflow[n] = reset_overflow[n];
    }

    /* initialise event listener structure */
    init_events();
    
    timestamp(&icomm->debug.ts_start);

    /* initialise bfaccess structure */
    memset(&bfaccess, 0, sizeof(bfaccess));
    bfaccess.fctrl = icomm->fctrl;
    bfaccess.overflow = icomm->overflow;
    bfaccess.realsize = bfconf->realsize;
    bfaccess.coeffs_data = bfconf->coeffs_data;
    bfaccess.control_mutex = icomm_mutex;
    bfaccess.reset_peak = bf_reset_peak;
    bfaccess.exit = bf_exit;
    bfaccess.toggle_mute = toggle_mute;
    bfaccess.ismuted = ismuted;
    bfaccess.set_delay = set_delay;
    bfaccess.get_delay = get_delay;
    bfaccess.realtime_index = bf_realtime_index;
    bfaccess.bfio_names = bfio_names;
    bfaccess.bfio_range = bfio_range;
    bfaccess.bfio_command = dai_subdev_command;
    bfaccess.bflogic_names = bflogic_names;
    bfaccess.bflogic_command = bflogic_command;
    bfaccess.convolver_coeffs2cbuf = convolver_runtime_coeffs2cbuf;
    bfaccess.convolver_fftplan = convolver_fftplan;
    bfaccess.set_subdelay = set_subdelay;
    bfaccess.get_subdelay = get_subdelay;

    /* create filter processes */
    cpos[IN] = cpos[OUT] = 0;
    for (n = 0; n < bfconf->n_processes; n++) {
	
	/* calculate how many (and which) inputs/outputs the process should
	   do FFTs for */
	FOR_IN_AND_OUT {
	    nc[IO] = bfconf->n_channels[IO] / bfconf->n_processes;
	    j = 0;
	    while ((j < nc[IO] || n == bfconf->n_processes - 1) &&
		   cpos[IO] < bfconf->n_physical_channels[IO])
	    {
		for (i = 0; i < bfconf->n_virtperphys[IO][cpos[IO]]; i++, j++) {
		    channels[IO][j] = bfconf->phys2virt[IO][cpos[IO]][i];
		}		
		cpos[IO]++;
	    }
	    nc[IO] = j;
	}        
	switch (pid = fork()) {
	case 0:
            
            close(bl_output_2_bl_input[0]);
            close(bl_output_2_bl_input[1]);
            close(bl_input_2_filter[1]);
            close(filter_2_bl_output[0]);
            close(cb_input_2_filter[1]);
            close(filter_2_cb_output[0]);
            for (i = 0; i < bfconf->n_processes; i++) {
                if (i == n) {
                    filter_writefd[i] = -1;
                    close(filter2filter_pipes[i][1]);
                } else {
                    close(filter2filter_pipes[i][0]);
                    filter_writefd[i] = filter2filter_pipes[i][1];
                }
            }

	    filter_process(&bfaccess,
                           buffers[IN],
			   buffers[OUT],
			   input_freqcbuf,
			   output_freqcbuf,
			   filter2filter_pipes[n][0],
			   filter_writefd,
			   bl_input_2_filter[0],
                           cb_input_2_filter[0],
			   filter_2_bl_output[1],
                           filter_2_cb_output[1],
			   nc[IN],
			   channels[IN],
			   nc[OUT],
			   channels[OUT],
			   bfconf->fproc[n].n_unique_channels[IN],
			   bfconf->fproc[n].unique_channels[IN],
			   bfconf->fproc[n].n_unique_channels[OUT],
			   bfconf->fproc[n].unique_channels[OUT],
			   bfconf->fproc[n].n_filters,
			   bfconf->fproc[n].filters,
			   n,
                           !!n_blocking_devs[IN],
                           !!n_blocking_devs[OUT],
                           !!n_callback_devs[IN],
                           !!n_callback_devs[OUT]);
	    /* never reached */
	    return;
	    
	case -1:
	    fprintf(stderr, "Fork failed: %s.\n", strerror(errno));
	    bf_exit(BF_EXIT_OTHER);
	    return;
	    
	default:
	    icomm->pids[icomm->n_pids] = pid;
	    icomm->n_pids += 1;
            break;
	}
    }
    close(bl_input_2_filter[0]);
    close(filter_2_bl_output[1]);
    close(cb_input_2_filter[0]);
    close(cb_input_2_filter[1]);
    close(filter_2_cb_output[0]);
    close(filter_2_cb_output[1]);
    for (n = 0; n < bfconf->n_processes; n++) {
        close(filter2filter_pipes[n][0]);
        close(filter2filter_pipes[n][1]);
    }
    
    for (n = 0; n < bfconf->n_logicmods; n++) {
	if (bfconf->logicmods[n].fork_mode != BF_FORK_DONT_FORK) {
            if (pipe(synch_pipe) == -1) {
                bf_exit(BF_EXIT_OTHER);
                return;
            }
	    switch (pid = fork()) {
	    case 0:
		if (bfconf->realtime_priority) {
		    switch (bfconf->logicmods[n].fork_mode) {
		    case BF_FORK_PRIO_MAX:
			bf_make_realtime(0, bfconf->realtime_usermaxprio,
                                         bfconf->logicnames[n]);
			break;
		    case BF_FORK_PRIO_FILTER:
			bf_make_realtime(0, bfconf->realtime_minprio,
                                         bfconf->logicnames[n]);
			break;
		    case BF_FORK_PRIO_OTHER:
			/* fall through */
		    default:
			break;
		    }			
		}                
                close(synch_pipe[0]);
                close(bl_output_2_bl_input[0]);
                close(bl_output_2_bl_input[1]);
                close(filter_2_bl_output[0]);
                close(bl_input_2_filter[1]);

		bfconf->logicmods[n].init(&bfaccess,
					  bfconf->sampling_rate,
					  bfconf->filter_length,
					  bfconf->n_blocks,
					  bfconf->n_coeffs,
					  bfconf->coeffs,
					  bfconf->n_channels,
					  (const struct bfchannel **)
					  bfconf->channels,
					  bfconf->n_filters,
					  bfconf->filters,
					  bfconf->logicmods[n].event_pipe[0],
                                          synch_pipe[1]);
		fprintf(stderr, "Forking logic module \"%s\": init function "
			"returned.\n", bfconf->logicnames[n]);
                bf_exit(BF_EXIT_OTHER);
		return;
		
	    case -1:
		fprintf(stderr, "Fork failed: %s.\n", strerror(errno));
                bf_exit(BF_EXIT_OTHER);
		return;
		
	    default:
		icomm->pids[icomm->n_pids] = pid;
		icomm->n_pids += 1;
                close(synch_pipe[1]);
                if (!readfd(synch_pipe[0], dummydata, 1)) {
                    bf_exit(BF_EXIT_OTHER);
                    return;
                }
                close(synch_pipe[0]);
	    }
	} else {
	    if (bfconf->logicmods[n].init(&bfaccess,
					  bfconf->sampling_rate,
					  bfconf->filter_length,
					  bfconf->n_blocks,
					  bfconf->n_coeffs,
					  bfconf->coeffs,
					  bfconf->n_channels,
					  (const struct bfchannel **)
					  bfconf->channels,
					  bfconf->n_filters,
					  bfconf->filters,
					  bfconf->logicmods[n].event_pipe[0],
                                          -1)
		== -1)
	    {
		fprintf(stderr, "Failed to init logic module \"%s\".\n",
			bfconf->logicnames[n]);
                bf_exit(BF_EXIT_OTHER);
		return;
	    }
	}
    }
    
    if (!bfconf->blocking_io) {
        /* no blocking I/O: finish startup, start callback I/O and exit */
        if (!writefd(bl_input_2_filter[1], dummydata, bfconf->n_processes) ||
            !readfd(filter_2_bl_output[0], dummydata, bfconf->n_processes))
        {
            fprintf(stderr, "Error: ran probably out of memory, aborting.\n");
            bf_exit(BF_EXIT_NO_MEMORY);
            return;
        }
        pinfo("Audio processing starts now\n");
        dai_trigger_callback_io();
        for (n = 0; n < icomm->n_pids; n++) {
            if (icomm->pids[n] == getpid()) {
                icomm->pids[n] = 0;
                break;
            }
        }
        exit(EXIT_SUCCESS);
    }

    if (n_blocking_devs[OUT] > 0) {
        /* create output process (if necessary) */
        pid = 0;
        if (n_blocking_devs[IN] > 0) {
            if (pipe(synch_pipe) == -1) {
                bf_exit(BF_EXIT_OTHER);
                return;
            }
            if ((pid = fork()) == -1) {
                fprintf(stderr, "Fork failed: %s.\n", strerror(errno));
                bf_exit(BF_EXIT_OTHER);
                return;
            }
            
        }
        if (pid == 0) {
            if (n_blocking_devs[IN] == 0) {
                if (!writefd(bl_input_2_filter[1], dummydata,
                             bfconf->n_processes))
                {
                    fprintf(stderr, "Error: ran probably out of memory, "
                            "aborting.\n");
                    bf_exit(BF_EXIT_NO_MEMORY);
                    return;
                }
            }
            if ((n_blocking_devs[IN] == 0 &&
                 !writefd(bl_input_2_filter[1], dummydata,
                          bfconf->n_processes)) ||
                !readfd(filter_2_bl_output[0], dummydata, bfconf->n_processes))
            {
                fprintf(stderr, "Error: ran probably out of memory, "
                        "aborting.\n");
                bf_exit(BF_EXIT_NO_MEMORY);
                return;
            }
            close(bl_input_2_filter[0]);
            close(bl_input_2_filter[1]);
            close(bl_output_2_bl_input[0]);
            close(synch_pipe[1]);
            checkdrift = true;
            FOR_IN_AND_OUT {
                for (n = 0; n < bfconf->n_subdevs[IO]; n++) {
                    if (bfconf->subdevs[IO][n].uses_clock) {
                        break;
                    }
                }
                if (n == bfconf->n_subdevs[IO]) {
                    checkdrift = false;
                }
            }
            if (n_callback_devs[IN] > 0 && n_blocking_devs[IN] > 0) {
                n = bl_output_2_bl_input[1];
                i = bl_output_2_cb_input[1];
            } else if (n_callback_devs[IN] > 0) {
                n = bl_output_2_cb_input[1];
                i = -1;
            } else {
                n = bl_output_2_bl_input[1];
                i = -1;
            }
            if (n_blocking_devs[IN] > 0) {
                j = synch_pipe[0];
                trigger = false;
            } else {
                close(synch_pipe[0]);
                j = -1;
                trigger = true;
            }
            output_process(filter_2_bl_output[0], j, n, i, trigger, checkdrift);
            /* never reached */
            return;
        } else {
            icomm->pids[icomm->n_pids] = pid;	
            icomm->n_pids += 1;
        }
    }

    /* start the input process (this code is reached only if necessary) */

    if (!writefd(bl_input_2_filter[1], dummydata, bfconf->n_processes) ||
        (n_blocking_devs[OUT] == 0 &&
         !readfd(filter_2_bl_output[0], dummydata, bfconf->n_processes)))
    {
        fprintf(stderr, "Error: ran probably out of memory, aborting.\n");
        bf_exit(BF_EXIT_NO_MEMORY);
        return;
    }
        
    close(filter_2_bl_output[0]);
    close(filter_2_bl_output[1]);    
    close(bl_output_2_bl_input[1]);
    close(synch_pipe[0]);
    
    if (n_callback_devs[OUT] > 0 && n_blocking_devs[OUT] > 0) {
        n = bl_output_2_bl_input[0];
        i = cb_output_2_bl_input[0];
    } else if (n_callback_devs[OUT] > 0) {
        n = cb_output_2_bl_input[0];
        i = -1;
    } else {
        n = bl_output_2_bl_input[0];
        i = -1;
    }
    if (n_blocking_devs[OUT] > 0) {
        j = synch_pipe[1];
    } else {
        close(synch_pipe[1]);
        j = -1;
    }
    input_process(buffers[IN], bl_input_2_filter[1], n, i, j);
    /* never reached */   
}

double
bf_realtime_index(void)
{
    return icomm->realtime_index;
}

void
bf_reset_peak(void)
{
    int n;
    
    icomm->doreset_overflow = true;
    for (n = 0; n < bfconf->n_channels[OUT]; n++) {
        icomm->overflow[n] = reset_overflow[n];
    }
}

int
bflogic_command(int modindex,
                const char params[],
                char **message)
{
    static char *msgstr = NULL;
    int ans;

    efree(msgstr);
    msgstr = NULL;
    if (modindex < 0 || modindex >= bfconf->n_logicmods) {
	if (message != NULL) {
	    msgstr = estrdup("Invalid module index");
	    *message = msgstr;
	}
	return -1;
    }
    if (params == NULL) {
	if (message != NULL) {
	    msgstr = estrdup("Missing parameters");
	    *message = msgstr;
	}
	return -1;
    }
    if (bfconf->logicmods[modindex].command == NULL) {
	if (message != NULL) {
	    msgstr = estrdup("Module does not support any commands");
	    *message = msgstr;
	}
	return -1;
    }
    ans = bfconf->logicmods[modindex].command(params);
    msgstr = estrdup(bfconf->logicmods[modindex].message());
    *message = msgstr;
    return ans;
}

const char **
bflogic_names(int *n_names)
{
    if (n_names != NULL) {
	*n_names = bfconf->n_logicmods;
    }
    return (const char **)bfconf->logicnames;
}

const char **
bfio_names(int io,
	   int *n_names)
{
    static char **names[2];
    static bool_t init = false;
    int n;
    
    if (io != IN && io != OUT) {
	return NULL;
    }    
    if (!init) {
	init = true;
	FOR_IN_AND_OUT {
	    names[IO] = emalloc(bfconf->n_subdevs[IO] * sizeof(char *));
	    for (n = 0; n < bfconf->n_subdevs[IO]; n++) {
		names[IO][n] = bfconf->ionames[bfconf->subdevs[IO][n].module];
	    }
	}
    }
    if (n_names != NULL) {
	*n_names = bfconf->n_subdevs[io];
    }
    return (const char **)names[io];
}

void
bfio_range(int io,
	   int modindex,
	   int range[2])
{
    if (range != NULL) {
	range[0] = 0;
	range[1] = 0;
    }
    if ((io != IN && io != OUT) || modindex < 0 ||
	modindex >= bfconf->n_subdevs[io] || range == NULL)
    {
	return;
    }
    range[0] = bfconf->subdevs[io][modindex].channels.channel_name[0];
    range[1] = bfconf->subdevs[io][modindex].channels.channel_name
	[bfconf->subdevs[io][modindex].channels.open_channels - 1];
}

void
bf_register_process(pid_t pid)
{
    icomm->pids[icomm->n_pids] = pid;	
    icomm->n_pids += 1;
}

void
bf_make_realtime(pid_t pid,
                 int priority,
                 const char name[])
{
    struct sched_param schp;

    if (icomm->ignore_rtprio) {        
        return;
    }
    
    memset(&schp, 0, sizeof(schp));
    schp.sched_priority = priority;

    if (sched_setscheduler(pid, SCHED_FIFO, &schp) != 0) {
        if (errno == EPERM) {
            pinfo("Warning: not allowed to set realtime priority. Will run "
                  "with default priority\n  instead, which is less "
                  "reliable (underflow may occur).\n");
            icomm->ignore_rtprio = true;
            return;
        } else {
            if (name != NULL) {
                fprintf(stderr, "Could not set realtime priority for %s "
                        "process: %s.\n", name, strerror(errno));
            } else {
                fprintf(stderr, "Could not set realtime priority: %s.\n",
                        strerror(errno));
            }
            bf_exit(BF_EXIT_OTHER);
        }
    }

    if (bfconf->lock_memory) {
#ifdef __OS_FREEBSD__
        pinfo("Warning: lock_memory not supported on this platform.\n");
#else        
        if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
            if (name != NULL) {
                fprintf(stderr, "Could not lock memory for %s process: %s.\n",
                        name, strerror(errno));
            } else {
                fprintf(stderr, "Could not lock memory: %s.\n",
                        strerror(errno));
            }
            bf_exit(BF_EXIT_OTHER);
        }
#endif        
    }
    if (name != NULL) {
        pinfo("Realtime priority %d set for %s process (pid %d)\n",
              priority, name, (int)getpid());
    }
}

void
bf_exit(int status)
{
    int n, self_pos = -1;
    pid_t self, other;

    self = getpid();
    
    if (icomm != NULL) {
        icomm->exit_status = status;
        for (n = 0; n < icomm->n_pids; n++) {
            if (icomm->pids[n] == self) {
                icomm->pids[n] = 0;
                self_pos = n;
            }
        }
	for (n = 0; n < icomm->n_pids; n++) {
	    if ((other = icomm->pids[n]) != 0) {
		kill(other, SIGTERM);
	    }
	}
    }
    dai_die();
    if (bfconf != NULL && bfconf->debug && self_pos == 0) {
        print_debug();
    }
    if (icomm == NULL) {
        exit(status);
    }
    exit(icomm->exit_status);
}
