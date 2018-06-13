/*
 * (c) Copyright 2001 - 2003, 2005 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _DAI_H_
#define _DAI_H_

#include <stdlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <inttypes.h>

#include "defs.h"
#include "inout.h"
#include "bfmod.h"

/* digital audio interface */

struct sample_format {
    bool_t isfloat;
    bool_t swap;
    int bytes;
    int sbytes;
    double scale;
    int format;
};

struct buffer_format {
    struct sample_format sf;
    int sample_spacing; /* in samples */
    int byte_offset;    /* in bytes */
};

struct dai_channels {
    struct sample_format sf;
    /* how many channels to open (on device level) */
    int open_channels;
    /* how many channels of the opened channels that are used (on device level)
       used_channels <= open_channels */    
    int used_channels;
    /* array (used_channels elements long) which contains the channel indexes
       on device level (0 <= index < open_channels) of the used channels. */
    int *channel_selection;
    /* array (used_channels elements long) which contains the channel indexes
       on a logical level. These indexes are those used in the dai_* function
       calls. */
    int *channel_name;
};

struct dai_subdevice {
    struct dai_channels channels;
    void *params;
    bool_t uses_clock;
    int sched_policy;
    struct sched_param sched_param;
    int module;
};

struct dai_buffer_format {
    int n_bytes;
    int n_samples;
    int n_channels;
    struct buffer_format bf[BF_MAXCHANNELS];
};

extern struct dai_buffer_format *dai_buffer_format[2];

struct debug_input {
    struct {
        uint64_t ts_start_call;
        uint64_t ts_start_ret;
    } init;
    struct {
        int fdmax;
        int retval;
        uint64_t ts_call;
        uint64_t ts_ret;
    } select;
    struct {
        int fd;
        void *buf;
        int offset;
        int count;
        int retval;
        uint64_t ts_call;
        uint64_t ts_ret;
    } read;
};

struct debug_output {
    struct {
        uint64_t ts_synchfd_call;
        uint64_t ts_synchfd_ret;
        uint64_t ts_start_call;
        uint64_t ts_start_ret;
    } init;
    struct {
        int fdmax;
        int retval;
        uint64_t ts_call;
        uint64_t ts_ret;
    } select;
    struct {
        int fd;
        void *buf;
        int offset;
        int count;
        int retval;
        uint64_t ts_call;
        uint64_t ts_ret;
    } write;
};

/*
 * The subdevs structures are used internally, so they must not be deallocated
 * nor modified.
 */
bool_t
dai_init(int period_size, /* in samples, must be a power of two */
	 int rate,
	 int n_subdevs[2],
	 struct dai_subdevice *subdevs[2],
         void *buffers[2][2]);

/*
 * Always deliver full fragment. If less than full (for files), it is handled
 * internally. (State is kept so not a full fragment is written on output).
 */
void
dai_input(volatile struct debug_input dbg[],
          int dbg_len,
          volatile int *dbg_loops);

/*
 * Always full fragment size. Buffer area not filled with samples must be zero.
 */
void
dai_output(bool_t iodelay_fill,
           int synch_fd,
           volatile struct debug_output dbg[],
           int dbg_len,
           volatile int *dbg_loops);

void
dai_trigger_callback_io(void);

int
dai_minblocksize(void);

bool_t
dai_input_poll_mode(void);

bool_t
dai_isinit(void);

void
dai_toggle_mute(int io,
		int channel);  

int
dai_change_delay(int io,
		 int channel,
		 int delay);

int
dai_subdev_command(int io,
                   int subdev_index,
                   const char params[],
                   char **message);

/*
 * Shut down prematurely. Must be called by the input and output process
 * separately. For any other process it has no effect.
 */
void
dai_die(void);

#endif
