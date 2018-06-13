/*
 * (c) Copyright 2001 - 2006, 2013, 2016 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sched.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#include "defs.h"
#include "emalloc.h"
#include "shmalloc.h"
#include "dai.h"
#include "bfrun.h"
#include "bit.h"
#include "inout.h"
#include "bfconf.h"
#include "timestamp.h"
#include "fdrw.h"
#include "pinfo.h"
#include "delay.h"
#include "timermacros.h"
#include "numunion.h"

/* FIXME: use bfconf directly in more situations? */

#define CB_MSG_START 1
#define CB_MSG_STOP 2

struct subdev {
    volatile bool_t finished;
    bool_t uses_callback;
    bool_t uses_clock;
    bool_t isinterleaved;
    bool_t bad_alignment;
    int index;
    int fd;
    int buf_size;
    int buf_offset;
    int buf_left;
    int block_size;
    int block_size_frames;
    struct dai_channels channels;
    delaybuffer_t **db;
    struct bfio_module *module;
    struct {
        int iodelay_fill;
        int curbuf;
        volatile int frames_left;
    } cb;
};

struct comarea {
    volatile bool_t blocking_stopped;
    volatile int lastbuf_index;
    volatile int frames_left;
    volatile int cb_lastbuf_index;
    volatile int cb_frames_left;
    volatile bool_t is_muted[2][BF_MAXCHANNELS];
    volatile int delay[2][BF_MAXCHANNELS];
    volatile pid_t pid[2];
    volatile pid_t callback_pid;
    struct subdev dev[2][BF_MAXCHANNELS];
    struct dai_buffer_format buffer_format[2];
    int buffer_id;
    volatile int cb_buf_index[2];
};

struct dai_buffer_format *dai_buffer_format[2] = { NULL, NULL };

static void *iobuffers[2][2];
static struct comarea *ca = NULL;
static int n_devs[2] = { 0, 0 };
static int n_fd_devs[2] = { 0, 0 };
static fd_set dev_fds[2];
static fd_set clocked_wfds;
static int n_clocked_devs = 0;
static int dev_fdn[2] = { 0, 0 };
static int min_block_size[2] = { 0, 0 };
static int cb_min_block_size[2] = { 0, 0 };
static bool_t input_poll_mode = false;
static struct subdev *dev[2][BF_MAXCHANNELS];
static struct subdev *fd2dev[2][FD_SETSIZE];
static struct subdev *ch2dev[2][BF_MAXCHANNELS];
static int period_size;
static int sample_rate;
static int monitor_rate_fd = -1;
static int synchpipe[2][2], paramspipe_s[2][2], paramspipe_r[2][2];
static int cbpipe_s[2], cbpipe_r[2];
static int cbmutex_pipe[2][2];
static int cbreadywait_pipe[2][2];
static volatile int callback_ready_waiting[2] = { 0, 0 };

static int
process_callback(void **state[2],
                 int state_count[2],
                 void **buffers[2],
                 int frame_count,
                 int event);
    
static void
cbmutex(int io,
        bool_t lock)
{
    char dummy = 0;

    if (lock) {
        if (!readfd(cbmutex_pipe[io][0], &dummy, 1)) {
            bf_exit(BF_EXIT_OTHER);
        }
    } else {
        if (!writefd(cbmutex_pipe[io][1], &dummy, 1)) {
            bf_exit(BF_EXIT_OTHER);
        }
    }
}

static bool_t
output_finish(void)
{
    bool_t finished;
    int n;

    cbmutex(OUT, true);
    finished = true;
    for (n = 0; n < n_devs[OUT]; n++) {
        if (!dev[OUT][n]->finished) {
            finished = false;
            break;
        }
    }
    if (finished) {
	pinfo("\nFinished!\n");
        return true;
    }
    cbmutex(OUT, false);
    return false;
}

static void
update_devmap(int idx,
	      int io)
{
    int n;

    if (dev[io][idx]->fd >= 0) {
        FD_SET(dev[io][idx]->fd, &dev_fds[io]);
        if (dev[io][idx]->fd > dev_fdn[io]) {
            dev_fdn[io] = dev[io][idx]->fd;
        }
        fd2dev[io][dev[io][idx]->fd] = dev[io][idx];
    }
    for (n = 0; n < dev[io][idx]->channels.used_channels; n++) {
	ch2dev[io][dev[io][idx]->channels.channel_name[n]] = dev[io][idx];
    }
}

/* if noninterleaved, update channel layout to fit the noninterleaved access
   mode (it is setup for interleaved layout per default). */
static void
noninterleave_modify(int idx,
		     int io)
{
    int n;
    
    if (!dev[io][idx]->isinterleaved) {
	dev[io][idx]->channels.open_channels =
	    dev[io][idx]->channels.used_channels;
	for (n = 0; n < dev[io][idx]->channels.used_channels; n++) {
	    dev[io][idx]->channels.channel_selection[n] = n;
	}
    }
}

static void
update_delay(struct subdev *sd,
             int io,
             uint8_t *buf)
{
    struct buffer_format *bf;
    int n, newdelay, virtch;

    if (sd->db == NULL) {
        return;
    }
    for (n = 0; n < sd->channels.used_channels; n++) {
        if (sd->db[n] == NULL) {
            continue;
        }
        bf = &dai_buffer_format[io]->bf[sd->channels.channel_name[n]];
        virtch = bfconf->phys2virt[io][sd->channels.channel_name[n]][0];
        newdelay = ca->delay[io][sd->channels.channel_name[n]];
        if (bfconf->use_subdelay[io] &&
            bfconf->subdelay[io][virtch] == BF_UNDEFINED_SUBDELAY)
        {
            newdelay += bfconf->sdf_length;
        }
        delay_update(sd->db[n], (void *)&buf[bf->byte_offset],
                     bf->sf.bytes, bf->sample_spacing, newdelay, NULL);
    }
}

static void
allocate_delay_buffers(int io,
		       struct subdev *sd)
{
    int n, virtch, extra_delay;

    sd->db = emalloc(sd->channels.used_channels *
		     sizeof(delaybuffer_t *));
    for (n = 0; n < sd->channels.used_channels; n++) {
	/* check if we need a delay buffer here, that is if at least one
	   channel has a direct virtual to physical mapping */
	if (bfconf->n_virtperphys[io][sd->channels.channel_name[n]] == 1) {
            virtch = bfconf->phys2virt[io][sd->channels.channel_name[n]][0];
            extra_delay = 0;
            if (bfconf->use_subdelay[io] &&
                bfconf->subdelay[io][virtch] == BF_UNDEFINED_SUBDELAY)
            {
                extra_delay = bfconf->sdf_length;
            }
	    sd->db[n] = delay_allocate_buffer(period_size,
					      bfconf->delay[io][virtch] +
                                              extra_delay,
					      bfconf->maxdelay[io][virtch] +
                                              extra_delay,
					      sd->channels.sf.bytes);
	} else {
	    /* this delay is taken care of previous to feeding the channel
	       output to this module */
	    sd->db[n] = NULL;
	}
    }
}

static void
do_mute(struct subdev *sd,
	int io,
	int wsize,
	void *_buf,
	int offset)
{
    int n, i, k, n_mute = 0, ch[sd->channels.used_channels], framesize;
    int bsch[sd->channels.used_channels], mid_offset;
    uint8_t *endp, *p, *startp;
    numunion_t *buf;
    uint16_t *p16;
    uint32_t *p32;
    uint64_t *p64;

    buf = (numunion_t *)_buf;
    /* Calculate which channels that should be cleared */
    for (n = 0; n < sd->channels.used_channels; n++) {
	if (ca->is_muted[io][sd->channels.channel_name[n]]) {
	    ch[n_mute] = sd->channels.channel_selection[n];
	    bsch[n_mute] = ch[n_mute] * sd->channels.sf.bytes;
	    n_mute++;
	}
    }
    if (n_mute == 0) {
	return;
    }

    if (!sd->isinterleaved) {
	offset /= sd->channels.open_channels;
	wsize /= sd->channels.open_channels;
        p = &buf->u8[offset];
	i = period_size * sd->channels.sf.bytes;
	for (n = 0; n < n_mute; n++) {
	    memset(p + ch[n] * i, 0, wsize);
	}
	return;
    }

    startp = &buf->u8[offset];
    endp = &buf->u8[offset] + wsize;
    framesize = sd->channels.open_channels * sd->channels.sf.bytes;
    mid_offset = offset;
    if ((i = offset % framesize) != 0) {
	for (k = 0; k < n_mute && bsch[k] + sd->channels.sf.bytes <= i; k++);
	for (n = i, p = startp;
	     p < startp + framesize - i && p < endp;
	     p++, n++)
	{
	    if (n >= bsch[k] && n < bsch[k] + sd->channels.sf.bytes) {
		*p = 0;
		if (n == bsch[k] + sd->channels.sf.bytes) {
		    if (++k == n_mute) {
			break;
		    }
		}
	    }
	}
	if (p == endp) {
	    return;
	}
	mid_offset += framesize - i;
    }

    switch (sd->channels.sf.bytes) {
    case 1:
	for (p = &buf->u8[mid_offset];
             p < endp;
             p += sd->channels.open_channels)
        {
	    for (n = 0; n < n_mute; n++) {
		p[ch[n]] = 0;
	    }
	}
	break;
    case 2:
	for (p16 = &buf->u16[mid_offset>>1];
             (uint8_t *)p16 < endp;
             p16 += sd->channels.open_channels)
        {
	    for (n = 0; n < n_mute; n++) {
		p16[ch[n]] = 0;
	    }
	}
	break;
    case 3:
	for (p = &buf->u8[mid_offset];
             p < endp;
             p += sd->channels.open_channels)
        {
	    for (n = 0; n < n_mute; n++) {
		p[bsch[n]+0] = 0;
		p[bsch[n]+1] = 0;
		p[bsch[n]+2] = 0;
	    }
	}
    case 4: {
	for (p32 = &buf->u32[mid_offset>>2];
             (uint8_t *)p32 < endp;
             p32 += sd->channels.open_channels)
        {
	    for (n = 0; n < n_mute; n++) {
		p32[ch[n]] = 0;
	    }
	}
	break;
    }
    case 8: {
	for (p64 = &buf->u64[mid_offset>>3];
             (uint8_t *)p64 < endp;
             p64 += sd->channels.open_channels)
        {
	    for (n = 0; n < n_mute; n++) {
		p64[ch[n]] = 0;
	    }
	}
	break;
    }
    default:
	fprintf(stderr, "Sample byte size %d not supported.\n",
		sd->channels.sf.bytes);
	bf_exit(BF_EXIT_OTHER);
	break;
    }
    
    if ((i = (offset + wsize) % framesize) != 0) {
	if ((p = endp - i) < startp) {
	    return;
	}	
	for (n = k = 0; p < endp; p++, n++) {
	    if (n >= bsch[k] && n < bsch[k] + sd->channels.sf.bytes) {
		*p = 0;
		if (n == bsch[k] + sd->channels.sf.bytes) {
		    if (++k == n_mute) {
			break;
		    }
		}
	    }
	}
    }
}

static bool_t
init_input(struct dai_subdevice *subdev,
	   int idx)
{
    int fd;
    
    dev[IN][idx]->uses_callback = bfconf->iomods[subdev->module].iscallback;
    dev[IN][idx]->channels = subdev->channels;
    dev[IN][idx]->uses_clock = subdev->uses_clock;
    dev[IN][idx]->module = &bfconf->iomods[subdev->module];
    dev[IN][idx]->index = idx;

    if ((fd = dev[IN][idx]->module->init(subdev->params,
                                         IN,
                                         subdev->channels.sf.format,
                                         sample_rate,
                                         subdev->channels.open_channels,
                                         subdev->channels.used_channels,
                                         subdev->channels.channel_selection,
                                         period_size,
                                         &dev[IN][idx]->block_size_frames,
                                         &dev[IN][idx]->isinterleaved,
                                         dev[IN][idx]->uses_callback ?
                                         dev[IN][idx] : NULL,
                                         dev[IN][idx]->uses_callback ?
                                         process_callback : NULL)) == -1)
    {
        fprintf(stderr, "Failed to init input device.\n");
	return false;
    }
    if (dev[IN][idx]->uses_callback) {
        dev[IN][idx]->fd = -1;
        if (dev[IN][idx]->block_size_frames == 0 ||
            period_size % dev[IN][idx]->block_size_frames != 0)
        {
            fprintf(stderr, "Invalid block size for callback input.\n");
            return false;
        }
        if (dev[IN][idx]->uses_clock &&
            (dev[IN][idx]->block_size_frames < cb_min_block_size[IN] ||
             cb_min_block_size[IN] == 0))
        {
            cb_min_block_size[IN] = dev[IN][idx]->block_size_frames;
        }
    } else {
        n_fd_devs[IN]++;
        dev[IN][idx]->fd = fd;
        if (bfconf->monitor_rate && monitor_rate_fd == -1 &&
            subdev->uses_clock)
        {
            monitor_rate_fd = dev[IN][idx]->fd;
        }
        if (dev[IN][idx]->uses_clock && dev[IN][idx]->block_size_frames != 0 &&
            (dev[IN][idx]->block_size_frames < min_block_size[IN] ||
             min_block_size[IN] == 0))
        {
            min_block_size[IN] = dev[IN][idx]->block_size_frames;
        }
    }
    dev[IN][idx]->isinterleaved = !!dev[IN][idx]->isinterleaved;
    if (dev[IN][idx]->uses_clock &&
        period_size % dev[IN][idx]->block_size_frames != 0)
    {
        dev[IN][idx]->bad_alignment = true;
    }
    noninterleave_modify(idx, IN);
    dev[IN][idx]->block_size = dev[IN][idx]->block_size_frames *
        dev[IN][idx]->channels.open_channels *
        dev[IN][idx]->channels.sf.bytes;
    allocate_delay_buffers(IN, dev[IN][idx]);
    update_devmap(idx, IN);
    return true;
}

static bool_t
init_output(struct dai_subdevice *subdev,
	    int idx)
{
    int fd;
    
    dev[OUT][idx]->uses_callback = bfconf->iomods[subdev->module].iscallback;
    dev[OUT][idx]->channels = subdev->channels;
    dev[OUT][idx]->uses_clock = subdev->uses_clock;
    dev[OUT][idx]->module = &bfconf->iomods[subdev->module];
    dev[OUT][idx]->index = idx;
    
    if ((fd = dev[OUT][idx]->module->init(subdev->params,
                                          OUT,
                                          subdev->channels.sf.format,
                                          sample_rate,
                                          subdev->channels.open_channels,
                                          subdev->channels.used_channels,
                                          subdev->channels.channel_selection,
                                          period_size,
                                          &dev[OUT][idx]->block_size_frames,
                                          &dev[OUT][idx]->isinterleaved,
                                          dev[OUT][idx]->uses_callback ?
                                          dev[OUT][idx] : NULL,
                                          dev[OUT][idx]->uses_callback ?
                                          process_callback : NULL)) == -1)
    {
        fprintf(stderr, "Failed to init output device.\n");
	return false;
    }
    if (dev[OUT][idx]->uses_callback) {
        dev[OUT][idx]->fd = -1;
        if (dev[OUT][idx]->block_size_frames == 0 ||
            period_size % dev[OUT][idx]->block_size_frames != 0)
        {
            fprintf(stderr, "Invalid block size for callback output.\n");
            return false;
        }
        if (dev[OUT][idx]->uses_clock &&
            (dev[OUT][idx]->block_size_frames < cb_min_block_size[OUT] ||
             cb_min_block_size[OUT] == 0))
        {
            cb_min_block_size[OUT] = dev[OUT][idx]->block_size_frames;
        }
    } else {
        n_fd_devs[OUT]++;
        dev[OUT][idx]->fd = fd;
        if (dev[OUT][idx]->uses_clock) {
            FD_SET(dev[OUT][idx]->fd, &clocked_wfds);
            n_clocked_devs++;
        }
        if (dev[OUT][idx]->uses_clock &&
            dev[OUT][idx]->block_size_frames != 0 &&
            (dev[OUT][idx]->block_size_frames < min_block_size[OUT] ||
             min_block_size[OUT] == 0))
        {
            min_block_size[OUT] = dev[OUT][idx]->block_size_frames;
        }
    }
    dev[OUT][idx]->isinterleaved = !!dev[OUT][idx]->isinterleaved;
    noninterleave_modify(idx, OUT);
    dev[OUT][idx]->block_size = dev[OUT][idx]->block_size_frames *
        dev[OUT][idx]->channels.open_channels *
        dev[OUT][idx]->channels.sf.bytes;
    allocate_delay_buffers(OUT, dev[OUT][idx]);
    update_devmap(idx, OUT);
    return true;
}

static void
calc_buffer_format(int fragsize,
		   int io,
		   struct dai_buffer_format *format)
{
    int n, i, ch;
    
    format->n_samples = fragsize;
    format->n_channels = 0;
    format->n_bytes = 0;
    for (n = 0; n < n_devs[io]; n++) {
	dev[io][n]->buf_offset = format->n_bytes;
	format->n_channels += dev[io][n]->channels.used_channels;
	for (i = 0; i < dev[io][n]->channels.used_channels; i++) {
	    ch = dev[io][n]->channels.channel_name[i];
	    format->bf[ch].sf = dev[io][n]->channels.sf;
	    if (dev[io][n]->isinterleaved) {
		format->bf[ch].byte_offset = format->n_bytes +
		    dev[io][n]->channels.channel_selection[i] *
		    dev[io][n]->channels.sf.bytes;
		format->bf[ch].sample_spacing =
		    dev[io][n]->channels.open_channels;
	    } else {
		format->bf[ch].byte_offset = format->n_bytes;
		format->bf[ch].sample_spacing = 1;
		format->n_bytes += dev[io][n]->channels.sf.bytes * fragsize;
	    }
	}
	dev[io][n]->buf_size = dev[io][n]->buf_left =
	    dev[io][n]->channels.open_channels *
	    dev[io][n]->channels.sf.bytes * fragsize;

	if (dev[io][n]->isinterleaved) {
	    format->n_bytes += dev[io][n]->buf_size;
	}
	if (format->n_bytes % ALIGNMENT != 0) {
	    format->n_bytes += ALIGNMENT - format->n_bytes % ALIGNMENT;
	}
    }
}

static void
handle_params(int io)
{
    char *params, *msgstr = NULL;
    int size, ans, subdev_index;
    
    if (!readfd(paramspipe_s[io][0], &subdev_index, sizeof(int))) {
	fprintf(stderr, "Failed to read from pipe.\n");
	bf_exit(BF_EXIT_OTHER);
    }
    if (!readfd(paramspipe_s[io][0], &size, sizeof(int))) {
        fprintf(stderr, "Failed to read from pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }
    if ((params = alloca(size)) == NULL) {
        fprintf(stderr, "Could not allocate %d bytes on stack.\n", size);
        bf_exit(BF_EXIT_OTHER);
    }
    if (!readfd(paramspipe_s[io][0], params, size)) {
        fprintf(stderr, "Failed to read from pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }	
    if (dev[io][subdev_index]->module->command == NULL) {
        ans = -1;
        msgstr = estrdup("Module does not support any commands");
    } else {
        ans = dev[io][subdev_index]->module->
            command(dev[io][subdev_index]->fd, params);
        msgstr = estrdup(dev[io][subdev_index]->module->message());
    }
    if (!writefd(paramspipe_r[io][1], &ans, sizeof(int))) {
        fprintf(stderr, "Failed to write to pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }
    size = strlen(msgstr) + 1;
    if (!writefd(paramspipe_r[io][1], &size, sizeof(int)) ||
        !writefd(paramspipe_r[io][1], msgstr, size))
    {
        fprintf(stderr, "Failed to write to pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }
    efree(msgstr);
}

static bool_t
callback_init(int n_subdevs[2],
              struct dai_subdevice *subdevs[2])
{
    int n;

    if (pipe(cbreadywait_pipe[IN]) == -1 ||
        pipe(cbreadywait_pipe[OUT]) == -1)
    {
        fprintf(stderr, "Failed to create pipe: %s.\n", strerror(errno));
        return false;
    }
    
    /* initialise inputs */
    for (n = 0; n < n_subdevs[IN]; n++) {
        if (!bfconf->iomods[subdevs[IN][n].module].iscallback) {
            continue;
        }
	if (!init_input(&subdevs[IN][n], n)) {
	    return false;
	}
    }    
    
    /* initialise outputs */
    for (n = 0; n < n_subdevs[OUT]; n++) {
        if (!bfconf->iomods[subdevs[OUT][n].module].iscallback) {
            continue;
        }
	if (!init_output(&subdevs[OUT][n], n)) {
	    return false;
	}
    }

    FOR_IN_AND_OUT {
        for (n = 0; n < n_subdevs[IO]; n++) {
            if (!bfconf->iomods[subdevs[IO][n].module].iscallback) {
                continue;
            }
            if (dev[IO][n]->bad_alignment) {
                fprintf(stderr, "\
Error: currently no support for bad callback I/O block alignment.\n\
  BruteFIR's partition length must be divisable with the sound server's\n\
  buffer size.\n");
                return false;
            }
        }
    }
    return true;
}

static void
callback_process(int n_subdevs[2],
                 struct dai_subdevice *subdevs[2],
                 pid_t wpid)
{
    uint8_t *buffer;
    char msg;
    int n;
    
    close(cbpipe_r[0]);
    close(cbpipe_s[1]);
    msg = (char)callback_init(n_subdevs, subdevs);
    writefd(cbpipe_r[1], &msg, 1);
    if (msg == 0) {
        while (true) sleep(1000);
    }
    readfd(cbpipe_s[0], &msg, 1);
    /* attach I/O buffers */
    if ((buffer = shmat(ca->buffer_id, NULL, 0)) == (void *)-1) {
        fprintf(stderr, "Failed to attach to shared memory with id %d: "
                "%s.\n", ca->buffer_id, strerror(errno));
        msg = 0;
    } else {
        FOR_IN_AND_OUT {
            iobuffers[IO][0] = buffer;
            buffer += ca->buffer_format[IO].n_bytes;
            iobuffers[IO][1] = buffer;
            buffer += ca->buffer_format[IO].n_bytes;
        }
        msg = 1;
    }
    writefd(cbpipe_r[1], &msg, 1);
    if (msg == 0) {
        while (true) sleep(1000);
    }
    if (bfconf->realtime_priority) {
        bf_make_realtime(getpid(), bfconf->realtime_midprio, "callback");
    }
    while (true) {
        if (!readfd(cbpipe_s[0], &msg, 1)) {
            break;
        }
        switch ((int)msg) {
        case CB_MSG_START:
            for (n = 0; n < bfconf->n_iomods; n++) {
                if (!bfconf->iomods[n].iscallback) {
                    continue;
                }
                if (bfconf->iomods[n].synch_start() != 0) {
                    fprintf(stderr, "Failed to start I/O module, aborting.\n");
                    bf_exit(BF_EXIT_OTHER);
                }
            }
            if (wpid > 0) {
                waitpid(wpid, &n, 0);
            }
            break;
        case CB_MSG_STOP:
            for (n = 0; n < bfconf->n_iomods; n++) {
                if (!bfconf->iomods[n].iscallback) {
                    continue;
                }
                bfconf->iomods[n].synch_stop();
            }
            msg = 1;
            writefd(cbpipe_r[1], &msg, 1);
            break;
        default:
            fprintf(stderr, "Bug: invalid msg %d, aborting.\n", (int)msg);
            bf_exit(BF_EXIT_OTHER);
            break;
        }
    }
    while (true) sleep(1000);
}

bool_t
dai_init(int _period_size,
	 int rate,
	 int n_subdevs[2],
	 struct dai_subdevice *subdevs[2],
         void *buffers[2][2])
{
    bool_t all_bad_alignment, none_clocked;
    uint8_t *buffer;
    char dummy = 0;
    int n;
    pid_t pid;

    memset(dev_fds, 0, sizeof(dev_fds));
    memset(&clocked_wfds, 0, sizeof(clocked_wfds));
    memset(fd2dev, 0, sizeof(fd2dev));
    memset(ch2dev, 0, sizeof(ch2dev));
    
    period_size = _period_size;
    sample_rate = rate;

    /* allocate shared memory for interprocess communication */
    if ((ca = shmalloc(sizeof(struct comarea))) == NULL) {
	fprintf(stderr, "Failed to allocate shared memory.\n");
	return false;
    }
    memset(ca, 0, sizeof(struct comarea));
    ca->frames_left = -1;
    ca->cb_frames_left = -1;
    FOR_IN_AND_OUT {
	dai_buffer_format[IO] = &ca->buffer_format[IO];
	n_devs[IO] = n_subdevs[IO];
	for (n = 0; n < bfconf->n_physical_channels[IO]; n++) {
	    if (bfconf->n_virtperphys[IO][n] == 1) {
		ca->delay[IO][n] =
		    bfconf->delay[IO][bfconf->phys2virt[IO][n][0]];
		ca->is_muted[IO][n] =
		    bfconf->mute[IO][bfconf->phys2virt[IO][n][0]];
	    } else {
		ca->delay[IO][n] = 0;
		ca->is_muted[IO][n] = false;
	    }
	}
        for (n = 0; n < n_devs[IO]; n++) {
            dev[IO][n] = &ca->dev[IO][n];
        }
    }

    if (pipe(cbpipe_s) == -1 ||
        pipe(cbpipe_r) == -1 ||
        pipe(cbmutex_pipe[IN]) == -1 ||
        pipe(cbmutex_pipe[OUT]) == -1)
    {
        fprintf(stderr, "Failed to create pipe: %s.\n", strerror(errno));
        return false;
    }
    if (!writefd(cbmutex_pipe[IN][1], &dummy, 1) ||
        !writefd(cbmutex_pipe[OUT][1], &dummy, 1))
    {
        return false;
    }
    
    /* initialise callback io, if any */
    if (bfconf->callback_io) {
        if ((pid = fork()) == -1) {
            fprintf(stderr, "Fork failed: %s.\n", strerror(errno));
            return false;
        }
        if (pid != 0) {
            bf_register_process(pid);
            ca->callback_pid = getpid();
            callback_process(n_subdevs, subdevs,
                             bfconf->blocking_io ? 0 : pid);
        }
        
        close(cbpipe_r[1]);
        close(cbpipe_s[0]);
        if (!readfd(cbpipe_r[0], &dummy, 1)) {
            return false;
        }
        if (dummy == 0) {
            return false;
        }
    }
    
    FOR_IN_AND_OUT {
	if (pipe(synchpipe[IO]) == -1 ||
	    pipe(paramspipe_s[IO]) == -1 ||
	    pipe(paramspipe_r[IO]) == -1)
	{
	    fprintf(stderr, "Failed to create pipe: %s.\n", strerror(errno));
	    return false;
	}
	if (!writefd(synchpipe[IO][1], &dummy, 1)) {
	    return false;
	}
    }
    
    /* initialise inputs */
    for (n = 0; n < n_subdevs[IN]; n++) {
        if (bfconf->iomods[subdevs[IN][n].module].iscallback) {
            continue;
        }
	if (!init_input(&subdevs[IN][n], n)) {
	    return false;
	}
    }    
    
    /* initialise outputs */
    for (n = 0; n < n_subdevs[OUT]; n++) {
        if (bfconf->iomods[subdevs[OUT][n].module].iscallback) {
            continue;
        }
	if (!init_output(&subdevs[OUT][n], n)) {
	    return false;
	}
    }

    /* calculate buffer format, and allocate buffers */
    FOR_IN_AND_OUT {
	calc_buffer_format(period_size, IO, &ca->buffer_format[IO]);
    }
    if ((buffer = shmalloc_id(&ca->buffer_id,
                              2 * dai_buffer_format[IN]->n_bytes +
                              2 * dai_buffer_format[OUT]->n_bytes)) == NULL)
    {
        fprintf(stderr, "Failed to allocate shared memory.\n");
        return false;
    }
    memset(buffer, 0, 2 * dai_buffer_format[IN]->n_bytes +
           2 * dai_buffer_format[OUT]->n_bytes);
    FOR_IN_AND_OUT {
        iobuffers[IO][0] = buffer;
        buffer += dai_buffer_format[IO]->n_bytes;
        iobuffers[IO][1] = buffer;
        buffer += dai_buffer_format[IO]->n_bytes;
        buffers[IO][0] = iobuffers[IO][0];
        buffers[IO][1] = iobuffers[IO][1];
    }
    if (bfconf->callback_io) {

        /* some magic callback I/O init values */
        for (n = 0; n < n_devs[OUT]; n++) {
            if (dev[OUT][n]->uses_callback) {
                dev[OUT][n]->buf_left = 0;
                dev[OUT][n]->cb.frames_left = -1;
                dev[OUT][n]->cb.iodelay_fill = 2 * period_size /
                    dev[OUT][n]->block_size_frames - 2;
            }            
        }
        
        /* let callback I/O attach shared mem buffers */
        if (!writefd(cbpipe_s[1], &dummy, 1) ||
            !readfd(cbpipe_r[0], &dummy, 1))
        {
            return false;
        }
        if (dummy == 0) {
            return false;
        }
    }

    /* decide if to use input poll mode */
    input_poll_mode = false;
    all_bad_alignment = true;
    none_clocked = true;
    for (n = 0; n < n_subdevs[IN]; n++) {
        if (dev[IN][n]->uses_clock && !dev[IN][n]->uses_callback) {
            none_clocked = false;
            if (!dev[IN][n]->bad_alignment) {
                all_bad_alignment = false;
            }
        }
    }
    if (bfconf->blocking_io && all_bad_alignment && !none_clocked) {
        if (!bfconf->allow_poll_mode) {
            fprintf(stderr, "\
Error: sound input hardware requires poll mode to be activated but current\n\
  configuration does not allow it (allow_poll_mode: false;).\n");
            return false;
        }
        input_poll_mode = true;
        pinfo("Input poll mode activated\n");
    }
    return true;
}

void
dai_trigger_callback_io(void)
{
    char msg;

    msg = CB_MSG_START;
    if (!writefd(cbpipe_s[1], &msg, 1)) {
        bf_exit(BF_EXIT_OTHER);
    }
}

int
dai_minblocksize(void)
{
    int size = 0;

    if (bfconf->blocking_io) {
        FOR_IN_AND_OUT {
            if (min_block_size[IO] != 0 &&
                (min_block_size[IO] < size || size == 0))
            {
                size = min_block_size[IO];
            }
        }
    }
    if (bfconf->callback_io) {
        FOR_IN_AND_OUT {
            if (cb_min_block_size[IO] != 0 &&
                (cb_min_block_size[IO] < size || size == 0))
            {
                size = cb_min_block_size[IO];
            }
        }
    }
    return size;
}

bool_t
dai_input_poll_mode(void)
{
    return input_poll_mode;
}

bool_t
dai_isinit(void)
{
    return (dai_buffer_format[IN] != NULL);
}

void
dai_toggle_mute(int io,
		int channel)
{
    if ((io != IN && io != OUT) || channel < 0 || channel >= BF_MAXCHANNELS) {
	return;
    }
    ca->is_muted[io][channel] = !ca->is_muted[io][channel];
}

int
dai_change_delay(int io,
		 int channel,
		 int delay)
{
    if (delay < 0 || channel < 0 || channel >= BF_MAXCHANNELS ||
        (io != IN && io != OUT) ||
        bfconf->n_virtperphys[io][channel] != 1)
    {
	return -1;
    }
    ca->delay[io][channel] = delay;	
    return 0;
}

int
dai_subdev_command(int io,
                   int subdev_index,
                   const char params[],
                   char **message)
{
    static char *msgstr = NULL;
    char dummy;
    int n, ans;

    efree(msgstr);
    msgstr = NULL;
    if (io != IN && io != OUT) {
	if (message != NULL) {
	    msgstr = estrdup("Invalid io selection");
	    *message = msgstr;
	}
	return -1;
    }
    if (subdev_index < 0 || subdev_index >= n_devs[io]) {
	if (message != NULL) {
	    msgstr = estrdup("Invalid device index");
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
    if (!readfd(synchpipe[io][0], &dummy, 1)) {
	fprintf(stderr, "Failed to read from pipe.\n");
	bf_exit(BF_EXIT_OTHER);
    }
    n = strlen(params) + 1;    
    if (!writefd(paramspipe_s[io][1], &subdev_index, sizeof(int)) ||
	!writefd(paramspipe_s[io][1], &n, sizeof(int)) ||
	!writefd(paramspipe_s[io][1], params, n))
    {
	fprintf(stderr, "Failed to write to pipe.\n");
	bf_exit(BF_EXIT_OTHER);
    }
    if (!readfd(paramspipe_r[io][0], &ans, sizeof(int))) {
	fprintf(stderr, "Failed to read from pipe.\n");
	bf_exit(BF_EXIT_OTHER);
    }
    if (!readfd(paramspipe_r[io][0], &n, sizeof(int))) {
        fprintf(stderr, "Failed to read from pipe.\n");
        bf_exit(BF_EXIT_OTHER);
    }
    msgstr = emalloc(n);
    if (!readfd(paramspipe_r[io][0], msgstr, n)) {
        fprintf(stderr, "Failed to read from pipe.\n");
	    bf_exit(BF_EXIT_OTHER);
    }
    if (message != NULL) {
        *message = msgstr;
    }
    if (!writefd(synchpipe[io][1], &dummy, 1)) {
	fprintf(stderr, "Failed to write to pipe.\n");
	bf_exit(BF_EXIT_OTHER);
    }
    return ans;
}

void
dai_die(void)
{
    bool_t iscallback;
    int n;
    
    if (ca == NULL) {
	return;
    }

    iscallback = (getpid() == ca->callback_pid);
    
    if (iscallback) {
        for (n = 0; n < bfconf->n_iomods; n++) {
            if (bfconf->iomods[n].iscallback) {                
                bfconf->iomods[n].synch_stop();
            }
        }
        return;
    } else if (ca->blocking_stopped) {
        return;
    }
    
    if (getpid() == ca->pid[OUT]) {
        for (n = 0; n < bfconf->n_iomods; n++) {
            if (!bfconf->iomods[n].iscallback &&
                bfconf->iomods[n].synch_stop != NULL)
            {
                bfconf->iomods[n].synch_stop();
            }
        }
    }
    FOR_IN_AND_OUT {
	if (getpid() == ca->pid[IO]) {
	    for (n = 0; n < bfconf->n_iomods; n++) {
                if (!bfconf->iomods[n].iscallback &&
                    bfconf->iomods[n].stop != NULL)
                {
                    bfconf->iomods[n].stop(IO);
                }
	    }
	}
    }
}

void
dai_input(volatile struct debug_input dbg[],
          int dbg_len,
          volatile int *dbg_loops)
{
    static bool_t isfirst = true, startmeasure = true;
    static int buf_index = 0, frames = 0, curbuf = 0;
    static struct timeval starttv;

    int fdn, fdmax, n, i, k, fd, devsleft, frames_left = 0, dbg_pos = 0;
    struct timeval tv, zerotv, *ptv;
    struct subdev *sd = NULL;
    double measured_rate;
    fd_set rfds, readfds;
    int64_t usec, usec2;
    struct timespec ts;
    bool_t firstloop;
    uint8_t *buf;
    int minleft;

    buf = (uint8_t *)iobuffers[IN][curbuf];
    curbuf = !curbuf;
    
    *dbg_loops = 0;
    zerotv.tv_sec = 0;
    zerotv.tv_usec = 0;
    firstloop = true;
    minleft = period_size;
    if ((ca->frames_left != -1 &&
         buf_index == ca->lastbuf_index + 1) ||
        (ca->cb_frames_left != -1 &&
         buf_index == ca->cb_lastbuf_index + 1))
    {
	for (n = 0; n < bfconf->n_iomods; n++) {
            if (bfconf->iomods[n].stop != NULL) {
                bfconf->iomods[n].stop(IN);
            }
	}
	/* There is no more data to read, just sleep and let the output process
	   end all processes. */
	while (true) sleep(100000);
    }
    devsleft = n_fd_devs[IN];
    memcpy(&rfds, &dev_fds[IN], sizeof(fd_set));

    if (isfirst) {
	ca->pid[IN] = getpid();
        
        timestamp(&dbg[0].init.ts_start_call);
        dai_trigger_callback_io();
	for (n = 0; n < bfconf->n_iomods; n++) {
            if (bfconf->iomods[n].iscallback) {
                continue;
            }
	    if ((bfconf->iomods[n].start != NULL &&
                 bfconf->iomods[n].start(IN) != 0) ||
                (bfconf->iomods[n].synch_start != NULL &&
                 bfconf->iomods[n].synch_start() != 0))
            {
		fprintf(stderr, "Failed to start I/O module, aborting.\n");
		bf_exit(BF_EXIT_OTHER);
	    }
	}
        timestamp(&dbg[0].init.ts_start_ret);
        
    }
    while (devsleft != 0) {
	memcpy(&readfds, &rfds, sizeof(fd_set));
	FD_SET(paramspipe_s[IN][0], &readfds);

        fdmax = (dev_fdn[IN] > paramspipe_s[IN][0]) ? dev_fdn[IN] :
            paramspipe_s[IN][0];

        dbg[dbg_pos].select.fdmax = fdmax;
        timestamp(&dbg[dbg_pos].select.ts_call);
        
        if (input_poll_mode) {
            if (!firstloop) {
                usec = (int64_t)minleft * 1000000 / (int64_t)sample_rate;
                if (min_block_size[IN] > 0) {
                    usec2 = (int64_t)min_block_size[IN] * 1000000
                        / (int64_t)sample_rate;
                    if (usec2 < usec) {
                        usec = usec2;
                    }
                }
                /* nanosleep sleeps precise in maximum 2 ms */
                if (usec > 40000) {
                    ts.tv_sec = 0;
                    ts.tv_nsec = usec * 1000;
                    nanosleep(&ts, NULL);
                } else if (usec > 20000) {
                    ts.tv_sec = 0;
                    ts.tv_nsec = 10000000;
                    nanosleep(&ts, NULL);
                } else if (usec > 2050) {
                    ts.tv_sec = 0;
                    ts.tv_nsec = 2000000;
                    nanosleep(&ts, NULL);
                } else if (usec > 50) {
                    ts.tv_sec = 0;
                    ts.tv_nsec = (usec - 50) * 1000;
                    nanosleep(&ts, NULL);
                }
            }
            ptv = &zerotv;
        } else {
            ptv = NULL;
        }

        while ((fdn = select(fdmax + 1, &readfds, NULL, NULL, ptv)) == -1 &&
               errno == EINTR);
        
        timestamp(&dbg[dbg_pos].select.ts_ret);
        dbg[dbg_pos].select.retval = fdn;
    
	if (fdn == -1) {
	    fprintf(stderr, "Select failed: %s.\n", strerror(errno));
	    bf_exit(BF_EXIT_OTHER);
	}

        for (n = 0; n < n_devs[IN]; n++) {
            if (dev[IN][n]->uses_clock && !dev[IN][n]->uses_callback &&
                !FD_ISSET(dev[IN][n]->fd, &readfds))
            {
                if (input_poll_mode || dev[IN][n]->bad_alignment) {
                    FD_SET(dev[IN][n]->fd, &readfds);
                    fdn++;
                }
            }
        }
	
	fd = -1;
	while (fdn--) {
            for (fd++; !FD_ISSET(fd, &readfds) && fd <= fdmax; fd++);
	    if (fd == paramspipe_s[IN][0]) {
		handle_params(IN);
		continue;
	    }
	    sd = fd2dev[IN][fd];

            dbg[dbg_pos].read.fd = fd;
            dbg[dbg_pos].read.buf = buf + sd->buf_offset;
            dbg[dbg_pos].read.offset = sd->buf_size - sd->buf_left;
            dbg[dbg_pos].read.count = sd->buf_left;
            timestamp(&dbg[dbg_pos].read.ts_call);

	    n = sd->module->read(fd, buf + sd->buf_offset, sd->buf_size -
				 sd->buf_left, sd->buf_left);

            timestamp(&dbg[dbg_pos].read.ts_ret);
            dbg[dbg_pos].read.retval = n;
            if (++dbg_pos == dbg_len) {
                dbg_pos = 0;
            }
            (*dbg_loops)++;
                
	    switch (n) {
	    case -1:
		switch (errno) {
		case EINTR:
		case EAGAIN:
		    /* try again later */
		    break;
		case EIO:
		    /* invalid input signal */
		    fprintf(stderr, "I/O module failed to read due to invalid "
			    "input signal, aborting.\n");
		    bf_exit(BF_EXIT_INVALID_INPUT);
		    break;
		case EPIPE:
		    /* buffer underflow */

                    /* Actually, this should be overflow, but since we have
                       linked the devices, broken pipe on output will be
                       noted on the input as well, and it is more likely that
                       it is an underflow on the output than an overflow on
                       the input */                    
		    fprintf(stderr, "I/O module failed to read (probably) due "
                            "to buffer underflow on output, aborting.\n");
		    bf_exit(BF_EXIT_BUFFER_UNDERFLOW);
		    break;
		default:
		    /* general error */
		    fprintf(stderr, "I/O module failed to read, aborting.\n");
		    bf_exit(BF_EXIT_OTHER);
		    break;
		}
		break;
		
	    case 0:
		if (sd->isinterleaved) {
		    memset(buf + sd->buf_offset + sd->buf_size - sd->buf_left,
			   0, sd->buf_left);
		} else {
		    i = sd->buf_size / sd->channels.open_channels;
		    k = sd->buf_left / sd->channels.open_channels;
		    for (n = 1; n <= sd->channels.open_channels; n++) {
			memset(buf + sd->buf_offset + n * i - k, 0, k);
		    }
		}
		devsleft--;
		FD_CLR(fd, &rfds);
		
		frames_left = (sd->buf_size - sd->buf_left) /
		    sd->channels.sf.bytes / sd->channels.open_channels;
		if (ca->frames_left == -1 || frames_left < ca->frames_left) {
		    ca->frames_left = frames_left;
		}
		ca->lastbuf_index = buf_index;
		break;
		
	    default:
		sd->buf_left -= n;
		if (monitor_rate_fd == fd) {
		    if (startmeasure) {
                        if (sd->buf_left == 0) {
                            startmeasure = false;
                            gettimeofday(&starttv, NULL);
                        }
		    } else {
			frames += n / (sd->buf_size / period_size);
                        if (frames >= sample_rate && sd->buf_left == 0) {
                            gettimeofday(&tv, NULL);
                            timersub(&tv, &starttv, &tv);
                            measured_rate = 1000.0 * (double)frames /
                                (double)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
                            if (bfconf->debug) {
                                fprintf(stderr, "measured rate: %.3f kHz "
                                        "(%d frames / %ld usecs)\n",
                                        measured_rate / 1000.0, frames,
                                        tv.tv_sec * 1000000 + tv.tv_usec);
                            }
                            if (measured_rate < (double)sample_rate * 0.98 ||
                                measured_rate > (double)sample_rate / 0.98)
                            {                            
                                fprintf(stderr, "Configured sample rate is "
                                        "%.1f kHz, but measured is %.1f kHz, "
                                        "aborting.\n",
                                        (double)sample_rate / 1000.0,
                                        measured_rate / 1000.0);
                                bf_exit(BF_EXIT_INVALID_INPUT);
                            }
                            startmeasure = true;
                            frames = 0;
                        }
		    }
		}
                n = sd->buf_left / (sd->buf_size / period_size);
                if (sd->uses_clock && (n < minleft || minleft == -1)) {
                    minleft = n;
                }                
		if (sd->buf_left == 0) {
		    sd->buf_left = sd->buf_size;
		    devsleft--;
		    FD_CLR(fd, &rfds);		
		}
		break;
	    }
	}
        firstloop = false;
    }
    
    for (n = 0; n < n_devs[IN]; n++) {
        sd = dev[IN][n];
        if (!sd->uses_callback) {
            do_mute(sd, IN, sd->buf_size, (void *)(buf + sd->buf_offset), 0);
            update_delay(sd, IN, buf);
        }
    }

    isfirst = false;

    buf_index++;
    
}

void
dai_output(bool_t iodelay_fill,
           int synch_fd,
           volatile struct debug_output dbg[],
           int dbg_len,
           volatile int *dbg_loops)
{
    static bool_t isfirst = true;
    static bool_t islast = false;
    static int buf_index = 0;
    static int curbuf = 0;
    static fd_set readfds;
    
    int devsleft, fdn, fd, n, frames_left;    
    uint8_t *buf, dummydata[1] = { '\0' };
    fd_set wfds, writefds;
    struct subdev *sd;
    int dbg_pos = 0;

    buf = (uint8_t *)iobuffers[OUT][curbuf];
    curbuf = !curbuf;
    
    *dbg_loops = 0;

    if ((ca->frames_left != -1 && buf_index == ca->lastbuf_index) ||
        (ca->cb_frames_left != -1 && buf_index == ca->cb_lastbuf_index))
    {
	frames_left = ca->frames_left;
        n = ca->cb_frames_left;
        if (frames_left == -1 || (n != -1 && n < frames_left)) {
            frames_left = n;
        }
	for (n = 0; n < n_devs[OUT]; n++) {
            if (!dev[OUT][n]->uses_callback) {
                dev[OUT][n]->buf_left = dev[OUT][n]->buf_size =
                    frames_left * dev[OUT][n]->channels.sf.bytes *
                    dev[OUT][n]->channels.open_channels;
            }
	}
        islast = true;
    }
    if (isfirst) {
	FD_ZERO(&readfds);
    }

    for (n = 0; n < n_devs[OUT]; n++) {
        if (dev[OUT][n]->uses_callback) {
            continue;
        }
        update_delay(dev[OUT][n], OUT, buf);
    }
    
    if (iodelay_fill) {
        memcpy(&wfds, &clocked_wfds, sizeof(fd_set));
        devsleft = n_clocked_devs;
    } else {
        devsleft = n_fd_devs[OUT];
        memcpy(&wfds, &dev_fds[OUT], sizeof(fd_set));
    }

    while (devsleft != 0) {
	memcpy(&writefds, &wfds, sizeof(fd_set));
	FD_SET(paramspipe_s[OUT][0], &readfds);
        fdn = (dev_fdn[OUT] > paramspipe_s[OUT][0]) ? dev_fdn[OUT] :
            paramspipe_s[OUT][0];
        
        dbg[dbg_pos].select.fdmax = fdn + 1;
        timestamp(&dbg[dbg_pos].select.ts_call);
        
	while ((fdn = select(fdn + 1, &readfds, &writefds, NULL, NULL)) == -1 &&
               errno == EINTR);
        
        timestamp(&dbg[dbg_pos].select.ts_ret);
        dbg[dbg_pos].select.retval = fdn;
        
	if (fdn == -1) {
	    fprintf(stderr, "Select failed: %s.\n", strerror(errno));
	    bf_exit(BF_EXIT_OTHER);
	}
        if (FD_ISSET(paramspipe_s[OUT][0], &readfds)) {
            handle_params(OUT);
            fdn--;
        }
        
	fd = -1;
	while (fdn--) {
            for (fd++; !FD_ISSET(fd, &writefds) && fd <= dev_fdn[OUT]; fd++);
	    sd = fd2dev[OUT][fd];
	    if (sd->block_size > 0 && sd->buf_left > sd->block_size) {
		n = sd->block_size + sd->buf_left % sd->block_size;
	    } else {
		n = sd->buf_left;
	    }
	    do_mute(sd, OUT, n, (void *)(buf + sd->buf_offset),
		    sd->buf_size - sd->buf_left);

            dbg[dbg_pos].write.fd = fd;
            dbg[dbg_pos].write.buf = buf + sd->buf_offset;
            dbg[dbg_pos].write.offset = sd->buf_size - sd->buf_left;
            dbg[dbg_pos].write.count = n;
            timestamp(&dbg[dbg_pos].write.ts_call);
            
	    n = sd->module->write(fd, buf + sd->buf_offset,
				  sd->buf_size - sd->buf_left, n);

            timestamp(&dbg[dbg_pos].write.ts_ret);
            dbg[dbg_pos].write.retval = n;
            if (++dbg_pos == dbg_len) {
                dbg_pos = 0;
            }
            (*dbg_loops)++;
    
	    switch (n) {
	    case -1:
                switch (errno) {
                case EINTR:
                case EAGAIN:
                    /* try again later */
                    break;
                case EPIPE:
                    /* buffer underflow */
                    fprintf(stderr, "I/O module failed to write due to buffer "
                            "underflow, aborting.\n");
                    bf_exit(BF_EXIT_BUFFER_UNDERFLOW);
                    break;
                default:
                    /* general error */
                    fprintf(stderr, "I/O module failed to write, aborting.\n");
                    bf_exit(BF_EXIT_OTHER);
                    break;
                }
		break;
		
	    default:
		sd->buf_left -= n;
		break;
	    }
	    if (sd->buf_left == 0) {
		sd->buf_left = sd->buf_size;
		devsleft--;
		FD_CLR(fd, &wfds);
	    }
	}
        if (synch_fd != -1) {
            timestamp(&dbg[0].init.ts_synchfd_call);
            if (!writefd(synch_fd, dummydata, 1)) {
                bf_exit(BF_EXIT_OTHER);
            }
            sched_yield(); /* let input process start now */
            timestamp(&dbg[0].init.ts_synchfd_ret);
            synch_fd = -1;
        }
	if (!iodelay_fill && isfirst) {
	    isfirst = false;
            
            timestamp(&dbg[0].init.ts_start_call);
	    for (n = 0; n < bfconf->n_iomods; n++) {
                if (bfconf->iomods[n].iscallback) {
                    continue;
                }
		if (bfconf->iomods[n].start != NULL &&
                    bfconf->iomods[n].start(OUT) != 0)
                {
		    fprintf(stderr, "I/O module failed to start, aborting.\n");
		    bf_exit(BF_EXIT_OTHER);
		}
	    }
            timestamp(&dbg[0].init.ts_start_ret);
	    ca->pid[OUT] = getpid();
	}
    }
    
    if (iodelay_fill) {
        return;
    }
    
    if (islast) {
	for (n = 0; n < bfconf->n_iomods; n++) {
            if (bfconf->iomods[n].iscallback) {
                /* callback I/O is stopped elsewhere */
                continue;
            }
            if (bfconf->iomods[n].synch_stop != NULL) {
                bfconf->iomods[n].synch_stop();
            }
            if (bfconf->iomods[n].stop != NULL) {
                bfconf->iomods[n].stop(OUT);
            }
	}
	ca->blocking_stopped = true;
        for (n = 0; n < n_devs[OUT]; n++) {
            sd = dev[OUT][n];
            if (!sd->uses_callback) {
                sd->finished = true;
            }
        }
        if (output_finish()) {
            bf_exit(BF_EXIT_OK);
        } else {
            while (true) sleep(1000);
        }
    }

    buf_index++;
}

static void
process_callback_input(struct subdev *sd,
                       void *cbbufs[],
                       int frame_count)
{
    struct buffer_format *bf;
    uint8_t *buf, *copybuf;
    int n, cnt, count;

    buf = (uint8_t *)iobuffers[IN][sd->cb.curbuf];

    count = frame_count * sd->channels.used_channels * sd->channels.sf.bytes;
    if (sd->isinterleaved) {
        memcpy(buf + sd->buf_offset + sd->buf_size - sd->buf_left,
               cbbufs[0], count);
    } else {
        bf = &dai_buffer_format[IN]->bf[sd->channels.channel_name[0]];
        cnt = count / sd->channels.used_channels;
        copybuf = buf + sd->buf_offset +
            (sd->buf_size - sd->buf_left) / sd->channels.used_channels;
        for (n = 0; n < sd->channels.used_channels; n++) {
            memcpy(copybuf, cbbufs[n], cnt);
            copybuf += period_size * bf->sf.sbytes;
        }
    }
    sd->buf_left -= count;
    if (sd->buf_left == 0) {
        sd->cb.curbuf = !sd->cb.curbuf;
        do_mute(sd, IN, sd->buf_size, (void *)(buf + sd->buf_offset), 0);
        update_delay(sd, IN, buf);
    }
}

static void
process_callback_output(struct subdev *sd,
                        void *cbbufs[],
                        int frame_count,
                        bool_t iodelay_fill)
{
    struct buffer_format *bf;
    uint8_t *buf, *copybuf;
    int n, cnt, count;

    buf = (uint8_t *)iobuffers[OUT][sd->cb.curbuf];

    count = frame_count * sd->channels.used_channels * sd->channels.sf.bytes;
    
    if (iodelay_fill) {
        if (sd->isinterleaved) {
            memset(cbbufs[0], 0, count);
        } else {
            cnt = count / sd->channels.used_channels;
            for (n = 0; n < sd->channels.used_channels; n++) {
                memset(cbbufs[n], 0, cnt);
            }
        }
        return;
    }

    if (sd->buf_left == sd->buf_size) {
        update_delay(sd, OUT, buf);
    }
    do_mute(sd, OUT, count, (void *)(buf + sd->buf_offset),
            sd->buf_size - sd->buf_left);
    if (sd->isinterleaved) {
        memcpy(cbbufs[0], buf + sd->buf_offset +
               sd->buf_size - sd->buf_left, count);
    } else {
        bf = &dai_buffer_format[OUT]->bf[sd->channels.channel_name[0]];
        cnt = count / sd->channels.used_channels;
        copybuf = buf + sd->buf_offset +
            (sd->buf_size - sd->buf_left) / sd->channels.used_channels;
        for (n = 0; n < sd->channels.used_channels; n++) {
            memcpy(cbbufs[n], copybuf, cnt);
            copybuf += period_size * bf->sf.sbytes;
        }
    }

    sd->buf_left -= count;
    if (sd->buf_left == 0) {
        sd->cb.curbuf = !sd->cb.curbuf;
    }
}

static void
trigger_callback_ready(int io)
{
    char dummy[BF_MAXCHANNELS];
    
    if (callback_ready_waiting[io] > 0) {
        memset(dummy, 0, sizeof(dummy));
        if (!writefd(cbreadywait_pipe[io][1], dummy,
                     callback_ready_waiting[io]))
        {
            bf_exit(BF_EXIT_OTHER);
        }
        callback_ready_waiting[io] = 0;
    }
    cbmutex(io, false);    
}

static void
wait_callback_ready(int io)
{
    char dummy;
    
    callback_ready_waiting[io]++;
    cbmutex(io, false);
    if (!readfd(cbreadywait_pipe[io][0], &dummy, 1)) {
        bf_exit(BF_EXIT_OTHER);
    }
}

static int
process_callback(void **states[2],
                 int state_count[2],
                 void **buffers[2],
                 int frame_count,
                 int event)
{
    bool_t finished, unlock_output;
    int n, i, buf_index;
    struct subdev *sd;

    switch (event) {
    case BF_CALLBACK_EVENT_LAST_INPUT:
        if (ca->cb_frames_left == -1 || frame_count < ca->cb_frames_left) {
            ca->cb_frames_left = frame_count;
        }
        ca->cb_lastbuf_index = ca->cb_buf_index[IN];
        return 0;
    case BF_CALLBACK_EVENT_FINISHED:
        for (n = 0; n < state_count[OUT]; n++) {
            sd = (struct subdev *)states[OUT][n];
            sd->finished = true;
        }
        cbmutex(IN, true);
        trigger_callback_ready(IN);
        cbmutex(OUT, true);
        trigger_callback_ready(OUT);
        if (output_finish()) {
            bf_exit(BF_EXIT_OK);
        }
        return -1;
    case BF_CALLBACK_EVENT_ERROR:
        fprintf(stderr, "An error occurred in a callback I/O module.\n");
        bf_exit(BF_EXIT_OTHER);
        break;
    case BF_CALLBACK_EVENT_NORMAL:
        break;
    default:
        fprintf(stderr, "Invalid event: %d\n", event);
        bf_exit(BF_EXIT_OTHER);
        break;
    }

    if (states == NULL || state_count == NULL || buffers == NULL ||
        frame_count <= 0)
    {
        fprintf(stderr, "Invalid parameters: states %p; state_count %p; "
                "buffers: %p; frame_count: %d\n", states, state_count, buffers,
                frame_count);
        bf_exit(BF_EXIT_OTHER);
    }

    if (state_count[IN] > 0) {

        cbmutex(IN, true);
        
        for (n = 0, i = 0; n < state_count[IN]; n++) {
            sd = (struct subdev *)states[IN][n];
            if (frame_count != sd->block_size_frames) {
                fprintf(stderr, "Error: unexpected callback I/O block "
                        "alignment (%d != %d)\n",
                        frame_count, sd->block_size_frames);
                bf_exit(BF_EXIT_OTHER);
            }
            process_callback_input(sd, &buffers[IN][i], frame_count);
            if (sd->isinterleaved) {
                i++;
            } else {
                i += sd->channels.used_channels;
            }
        }
        
        sd = (struct subdev *)states[IN][0]; 
        if (sd->buf_left == 0) {
            finished = true;
            for (n = 0; n < n_devs[IN]; n++) {
                sd = dev[IN][n];
                if (sd->uses_callback && sd->buf_left != 0) {
                    finished = false;
                    break;
                }
            }
            if (finished) {
                for (n = 0; n < n_devs[IN]; n++) {
                    sd = dev[IN][n];
                    if (sd->uses_callback) {
                        sd->buf_left = sd->buf_size;
                    }
                }
                bf_callback_ready(IN);
                ca->cb_buf_index[IN]++;
                trigger_callback_ready(IN);
            } else {
                wait_callback_ready(IN);
            }
        } else {
            cbmutex(IN, false);
        }
    }
    
    if (state_count[OUT] > 0) {

        cbmutex(OUT, true);

        unlock_output = false;
        sd = (struct subdev *)states[OUT][0]; 
        if (sd->buf_left == 0 && sd->cb.iodelay_fill == 0) {
            finished = true;
            for (n = 0; n < n_devs[OUT]; n++) {
                sd = dev[OUT][n];
                if (sd->uses_callback &&
                    (sd->buf_left != 0 || sd->cb.iodelay_fill != 0))
                {
                    finished = false;
                    break;
                }
            }
            if (finished) {
                for (n = 0; n < n_devs[OUT]; n++) {
                    sd = dev[OUT][n];
                    if (sd->uses_callback) {
                        sd->buf_left = sd->buf_size;
                    }
                }
                bf_callback_ready(OUT);
                ca->cb_buf_index[OUT]++;
                trigger_callback_ready(OUT);
            } else {
                wait_callback_ready(OUT);
            }
        } else {
            unlock_output = true;
        }

        for (n = 0, i = 0; n < state_count[OUT]; n++) {
            sd = (struct subdev *)states[OUT][n];
            if (frame_count != sd->block_size_frames) {
                fprintf(stderr, "Error: unexpected callback I/O block "
                        "alignment (%d != %d)\n",
                        frame_count, sd->block_size_frames);
                bf_exit(BF_EXIT_OTHER);
            }
            process_callback_output(sd, &buffers[OUT][i], frame_count,
                                    sd->cb.iodelay_fill != 0);
            if (sd->cb.iodelay_fill != 0) {
                sd->cb.iodelay_fill--;
            }
            if (sd->isinterleaved) {
                i++;
            } else {
                i += sd->channels.used_channels;
            }
        }

        if (unlock_output) {
            cbmutex(OUT, false);
        }

        /* last buffer? */
        n = ca->cb_buf_index[IN];
        i = ca->cb_buf_index[OUT];
        buf_index = n < i ? i : n;
        sd = (struct subdev *)states[OUT][0];
        if (sd->cb.frames_left == -1 &&
            ((ca->frames_left != -1 && buf_index == ca->lastbuf_index + 1) ||
             (ca->cb_frames_left != -1 &&
              buf_index == ca->cb_lastbuf_index + 1)))
        {        
            n = ca->frames_left;
            i = ca->cb_frames_left;
            if (n == -1 || (n > i && i != -1)) {
                n = i;
            }
            sd->cb.frames_left = n;
        }    

        if (sd->cb.frames_left != -1) {
            if (sd->cb.frames_left > sd->block_size_frames) {
                sd->cb.frames_left -= sd->block_size_frames;
                return 0;
            }
            if (sd->cb.frames_left == 0) {
                return -1;
            }
            return sd->cb.frames_left;
        }
        
    }
    
    return 0;
}
