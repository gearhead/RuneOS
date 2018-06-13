/*
 * (c) Copyright 2001 - 2005, 2009, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#define index _index /* hack to avoid shadowing of index() */
#define ALSA_PCM_NEW_HW_PARAMS_API
#define ALSA_PCM_NEW_SW_PARAMS_API
#include <alsa/asoundlib.h>
#undef index

#include "defs.h"
#include "log2.h"
#include "bit.h"
#include "emalloc.h"
#define IS_BFIO_MODULE
#include "bfmod.h"
#include "inout.h"

#define ERRORSIZE 1024

struct alsa_access_state {
    snd_pcm_t *handle;
    bool_t isinterleaved;
    bool_t ignore_xrun;
    int sw_period_size;
    int sample_size;
    int used_channels;
    int open_channels;
    /* This union is a hack to be able to 'cast' between const and non const
       without the compiler noticing and complaining about it. This is required
       because ALSA's write functions do not have const buffers for input */
    union {
        void **ptr;
        const void **const_ptr;
    } bufs;
    int *channel_selection;
    char *device;
    bool_t restart;
};

struct settings {
    bool_t ignore_xrun;
    char *device;
};

static snd_pcm_t *handles[2][BF_MAXCHANNELS];
static int n_handles[2];
static struct alsa_access_state fd2as[FD_SETSIZE];
static snd_pcm_t *base_handle = NULL;
static bool_t debug = false;
static bool_t link_handles = true;
static snd_output_t *out;

static bool_t
set_params(snd_pcm_t *handle,
	   int sample_format,
	   int sample_rate,
	   int open_channels,
	   int period_size,
	   int *hardware_period_size,
	   int *sample_size,
	   bool_t *isinterleaved,
	   char errstr[])
{
    snd_pcm_uframes_t try_hw_period_size, hw_period_size, frames;
    snd_pcm_sw_params_t *swparams;
    snd_pcm_hw_params_t *params;
    int format, err;
    unsigned int un;

    if (log2_get(period_size) == -1) {
	sprintf(errstr, "  Invalid software period size (%d): must be a power "
		"of 2.\n", period_size);
	return false;
    }
    
    switch (sample_format) {
    case BF_SAMPLE_FORMAT_S8:
	format = SND_PCM_FORMAT_S8;
	*sample_size = 1;
	break;
    case BF_SAMPLE_FORMAT_S16_LE:
	format = SND_PCM_FORMAT_S16_LE;
	*sample_size = 2;
	break;
    case BF_SAMPLE_FORMAT_S16_BE:
	format = SND_PCM_FORMAT_S16_BE;
	*sample_size = 2;
	break;
    case BF_SAMPLE_FORMAT_S24_LE:
	format = SND_PCM_FORMAT_S24_3LE;
	*sample_size = 3;
	break;
    case BF_SAMPLE_FORMAT_S24_BE:
	format = SND_PCM_FORMAT_S24_3BE;
	*sample_size = 3;
	break;
    case BF_SAMPLE_FORMAT_S24_4LE:
	format = SND_PCM_FORMAT_S24_LE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_S24_4BE:
	format = SND_PCM_FORMAT_S24_BE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_S32_LE:
	format = SND_PCM_FORMAT_S32_LE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_S32_BE:
	format = SND_PCM_FORMAT_S32_BE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_FLOAT_LE:
	format = SND_PCM_FORMAT_FLOAT_LE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_FLOAT_BE:
	format = SND_PCM_FORMAT_FLOAT_BE;
	*sample_size = 4;
	break;
    case BF_SAMPLE_FORMAT_FLOAT64_LE:
	format = SND_PCM_FORMAT_FLOAT64_LE;
	*sample_size = 8;
	break;
    case BF_SAMPLE_FORMAT_FLOAT64_BE:
	format = SND_PCM_FORMAT_FLOAT64_BE;
	*sample_size = 8;
	break;
    default:
	sprintf(errstr, "  Unsupported sample format.\n");
	return false;
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_sw_params_alloca(&swparams);
    if ((err = snd_pcm_hw_params_any(handle, params)) < 0) {
	sprintf(errstr, "  Could not get any hardware configuration: %s.\n",
		snd_strerror(err));
	return false;
    }
    
    if (snd_pcm_hw_params_set_access(handle, params,
				     SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
    {
	if ((err =
	     snd_pcm_hw_params_set_access(handle, params,
					  SND_PCM_ACCESS_RW_NONINTERLEAVED))
	    < 0)
	{
	    sprintf(errstr, "  Failed to set interleaved and non-interleaved "
		    "access mode: %s.\n", snd_strerror(err));
	    return false;
	}
	*isinterleaved = false;
    } else {
	*isinterleaved = true;
    }

    /* It seems like it is best to set_rate_near instead of exact, have had
       problems with ens1371 */
    un = sample_rate;
    if ((err = snd_pcm_hw_params_set_rate_near(handle, params, &un, NULL)) < 0) {
	sprintf(errstr, "  Failed to set sample rate to %d Hz: %s.\n",
		sample_rate, snd_strerror(err));
	return false;
    }    
    /* accept a minor variation in sample rate */
    if (un != sample_rate && !((int)((double)sample_rate * 0.99) < un &&
                               (int)((double)sample_rate / 0.99) > un))
    {
        sprintf(errstr, "  Failed to set sample rate to %d Hz, device "
                "suggested %u Hz instead.\n", sample_rate, un);
        return false;
    }
    
    if ((err = snd_pcm_hw_params_set_format(handle, params, format)) < 0) {
	sprintf(errstr, "  Failed to set sample format to %s: %s.\n",
		bf_strsampleformat(sample_format), snd_strerror(err));
	return false;
    }
    if ((err = snd_pcm_hw_params_set_channels(handle, params,
					      open_channels)) < 0)
    {
	sprintf(errstr, "  Failed to set channel count to %d: %s.\n",
		open_channels, snd_strerror(err));
	return false;
    }    

    snd_pcm_hw_params_get_periods_max(params, &un, NULL);
    if (un < 2) {
	/* really strange hardware if this happens */
	sprintf(errstr,
"  Hardware does not support enough periods. At least 2 is required, but the\n\
  hardware supports only %u.\n", un);
	return false;	
    }
    
    /* try to get a hardware fragment size close to the software size */
    hw_period_size = period_size;
    snd_pcm_hw_params_set_period_size_near(handle, params, &hw_period_size,
                                           NULL);
    /* if the number of periods is only one, decrease the period size until we
       get at least two periods */
    snd_pcm_hw_params_get_periods(params, &un, NULL);
    try_hw_period_size = hw_period_size;
    while (un == 1 && try_hw_period_size != 0) {
	try_hw_period_size /= 2;
	hw_period_size = try_hw_period_size;
	snd_pcm_hw_params_set_period_size_near(handle, params,
					       &hw_period_size, NULL);
        snd_pcm_hw_params_get_periods(params, &un, NULL);
    }
    if (hw_period_size == 0) {
	/* this should never happen, since we have checked that the hardware
	   supports at least two periods */
	sprintf(errstr, "  Could not set period size.\n");
	return false;
    }
    if (snd_pcm_hw_params(handle, params) < 0) {
	sprintf(errstr, "  Unable to install hw params.\n");
	return false;
    }
    /* configure to start when explicitly told so */
    snd_pcm_sw_params_current(handle, swparams);
    if ((err = snd_pcm_sw_params_set_start_threshold(handle, swparams,
						     ~0U)) < 0)
    {
	sprintf(errstr, "  Failed to set start threshold: %s.\n",
		snd_strerror(err));
	return false;
    }

    /* configure to stop when buffer underflow is detected */
    snd_pcm_hw_params_get_buffer_size(params, &frames);
    if ((err = snd_pcm_sw_params_set_stop_threshold(handle, swparams,
                                                    frames)) < 0)
    {
        sprintf(errstr, "  Failed to set stop threshold: %s.\n",
                snd_strerror(err));
        return false;
    }
    
    snd_pcm_hw_params_get_period_size(params, &hw_period_size, NULL);
    *hardware_period_size = (int)hw_period_size;
    if ((err = snd_pcm_sw_params_set_avail_min(handle, swparams, 1)) < 0) {
        sprintf(errstr, "  Failed to set min avail to 1: %s.\n",
                snd_strerror(err));
        return false;
    }
    
    if ((err = snd_pcm_sw_params(handle, swparams)) < 0) {
	sprintf(errstr, "  Unable to install sw params: %s.\n",
		snd_strerror(err));
	return false;
    }
    if ((err = snd_pcm_prepare(handle)) < 0) {
	sprintf(errstr, "  Unable to prepare audio: %s.\n", snd_strerror(err));
	return false;
    }
    
    if (debug) {
        snd_pcm_dump(handle, out);
    }
    
    return true;
}

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "ALSA I/O: Parse error: " errstr);                     \
        return NULL;                                                           \
    }

void *
bfio_preinit(int *version_major,
             int *version_minor,
             int (*get_config_token)(union bflexval *lexval),
             int io,
             int *sample_format,
             int sample_rate,
             int open_channels,
             int *uses_sample_clock,
             int *callback_sched_policy,
             struct sched_param *callback_sched,
             int _debug)
{
    static bool_t has_been_called = false;
    
    struct settings *settings;
    union bflexval lexval;
    int err, token, ver;

    ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return NULL;
    }
    debug = !!_debug;
    
    if (!has_been_called &&
        (err = snd_output_stdio_attach(&out, stderr, 0)) != 0)
    {
	fprintf(stderr, "ALSA I/O: Unable to attach output: %s.\n",
                snd_strerror(err));
        return NULL;
    }   

    settings = malloc(sizeof(struct settings));
    memset(settings, 0, sizeof(struct settings));
    while ((token = get_config_token(&lexval)) > 0) {
        if (token != BF_LEXVAL_FIELD) {
            fprintf(stderr, "ALSA I/O: Parse error: expected field.\n");
            return NULL;
        }
        if (strcmp(lexval.field, "param") == 0 || /* param for compability */
            strcmp(lexval.field, "device") == 0)
        {
            if (settings->device != NULL) {
                fprintf(stderr, "ALSA I/O: Parse error: device already set.\n");
                return NULL;
            }
            GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
            settings->device = estrdup(lexval.string);
        } else if (strcmp(lexval.field, "ignore_xrun") == 0) {
            GET_TOKEN(BF_LEXVAL_BOOLEAN, "expected boolean value.\n");
            settings->ignore_xrun = lexval.boolean;
        } else if (strcmp(lexval.field, "link") == 0) {
            GET_TOKEN(BF_LEXVAL_BOOLEAN, "expected boolean value.\n");
            if (has_been_called && lexval.boolean != link_handles) {
                fprintf(stderr, "ALSA I/O: \"link\" is a global setting, "
                        "if set on more than one device, the\n  value must "
                        "be the same.\n");
                return NULL;
            }
            link_handles = lexval.boolean;
        } else {
            fprintf(stderr, "ALSA I/O: Parse error: unknown field.\n");
            return NULL;
        }
        GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
    }    
    if (settings->device == NULL) {
        fprintf(stderr, "ALSA I/O: Parse error: device not set.\n");
        return NULL;
    }
    if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
        fprintf(stderr, "ALSA I/O: No support for AUTO sample format.\n");
        return NULL;
    }
    
    *uses_sample_clock = 1;

    has_been_called = true;
    return settings;
}

int
bfio_init(void *params,
	  int io,
	  int sample_format,
	  int sample_rate,
	  int open_channels,
	  int used_channels,
	  const int channel_selection[],
	  int period_size,
	  int *device_period_size,
	  int *isinterleaved,
          void *callback_state,
          int (*process_callback)(void **callback_states[2],
                                  int callback_state_count[2],
                                  void **buffers[2],
                                  int frame_count,
                                  int event))
{
    struct settings *settings;
    char errstr[ERRORSIZE];
    struct pollfd pollfd;
    int err, sample_size;
    snd_pcm_t *handle;

    settings = (struct settings *)params;
    if ((err = snd_pcm_open(&handles[io][n_handles[io]], settings->device,
			    (io == BF_IN) ? SND_PCM_STREAM_CAPTURE :
			    SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0)
    {
	fprintf(stderr, "ALSA I/O: Could not open audio %s \"%s\": %s.\n",
		io == BF_IN ? "input" : "output", settings->device,
		snd_strerror(err));
	return -1;
    }

    handle = handles[io][n_handles[io]];

    if (!set_params(handle, sample_format, sample_rate, open_channels,
		    period_size, device_period_size, &sample_size,
                    isinterleaved, errstr))
    {
	fprintf(stderr, "ALSA I/O: Could not set audio %s parameters for "
                "\"%s\":\n%s",
		io == BF_IN ? "input" : "output", settings->device, errstr);
	snd_pcm_close(handle);
	return -1;
    }
    if (snd_pcm_poll_descriptors(handle, &pollfd, 1) != 1) {
	fprintf(stderr, "ALSA I/O: Could not get file descriptor.\n");
	snd_pcm_close(handle);
	return -1;
    }
    if (base_handle == NULL) {
        base_handle = handle;
    } else if (link_handles) {
	if ((err = snd_pcm_link(base_handle, handle)) < 0) {
	    fprintf(stderr, "ALSA I/O: Could not link alsa devices: %s.\n",
		    snd_strerror(err));
	    snd_pcm_close(handle);
	    return -1;
	}
    }
    n_handles[io]++;
        
    fd2as[pollfd.fd].handle = handle;
    fd2as[pollfd.fd].isinterleaved = *isinterleaved;
    fd2as[pollfd.fd].ignore_xrun = settings->ignore_xrun;
    fd2as[pollfd.fd].sw_period_size = period_size;
    fd2as[pollfd.fd].sample_size = sample_size;
    fd2as[pollfd.fd].open_channels = open_channels;
    fd2as[pollfd.fd].used_channels = used_channels;
    fd2as[pollfd.fd].device = settings->device;
    if (*isinterleaved) {
	fd2as[pollfd.fd].bufs.ptr = NULL;
	fd2as[pollfd.fd].channel_selection = NULL;
    } else {
	fd2as[pollfd.fd].bufs.ptr = emalloc(open_channels * sizeof(void *));
	memset(fd2as[pollfd.fd].bufs.ptr, 0, open_channels * sizeof(void *));
	fd2as[pollfd.fd].channel_selection =
            emalloc(used_channels * sizeof(int));
	memcpy(fd2as[pollfd.fd].channel_selection, channel_selection,
	       used_channels * sizeof(int));
    }
    efree(settings->device);
    efree(settings);
    return pollfd.fd;
}

int
bfio_synch_start(void)
{
    snd_pcm_status_t *status;
    int err, n;
    
    if (base_handle == NULL) {
        return 0;
    }

    /* FIXME: the SND_PCM_STATE_RUNNING code would not be needed if the
       bfio_write autostart hack was not there */
    snd_pcm_status_alloca(&status);

    if (link_handles) {
        if ((err = snd_pcm_status(base_handle, status)) < 0) {
            fprintf(stderr, "ALSA I/O: Could not get status: %s.\n",
                    snd_strerror(err));
            return -1;
        }
        if (snd_pcm_status_get_state(status) == SND_PCM_STATE_RUNNING) {
            return 0;
        }    
        if ((err = snd_pcm_start(base_handle)) < 0) {
            fprintf(stderr, "ALSA I/O: Could not start audio: %s.\n",
                    snd_strerror(err));
            return -1;
        }
        return 0;
    }
        
    FOR_IN_AND_OUT {
        for (n = 0; n < n_handles[IO]; n++) {
            if ((err = snd_pcm_status(handles[IO][n], status)) < 0) {
                fprintf(stderr, "ALSA I/O: Could not get status: %s.\n",
                        snd_strerror(err));
                return -1;
            }
            if (snd_pcm_status_get_state(status) == SND_PCM_STATE_RUNNING) {
                continue;
            }
            
            if ((err = snd_pcm_start(handles[IO][n])) < 0) {
                fprintf(stderr, "ALSA I/O: Could not start audio: %s.\n",
                        snd_strerror(err));
                return -1;
            }
        }
    }
    return 0;
}

void
bfio_synch_stop(void)
{
    int n;

    if (base_handle == NULL) {
        return;
    }
    FOR_IN_AND_OUT {
        for (n = 0; n < n_handles[IO]; n++) {
            snd_pcm_close(handles[IO][n]);
        }
    }
}

int
bfio_read(int fd,
	  void *buf,
	  int offset,
	  int count)
{
    struct alsa_access_state *as = &fd2as[fd];
    int n, i, err;
    uint8_t *ptr;

 bfio_read_restart:
    
    if (as->isinterleaved) {
	i = as->sample_size * as->open_channels;	
	if ((n = snd_pcm_readi(as->handle, &((uint8_t *)buf)[offset],
                               count / i)) < 0)
        {
            goto bfio_read_error;
	}
    } else {    
        ptr = (uint8_t *)buf;
        ptr += offset / as->used_channels;
        for (n = 0; n < as->used_channels; n++) {
            as->bufs.ptr[as->channel_selection[n]] = ptr;
            ptr += as->sw_period_size * as->sample_size;
        }
        i = as->sample_size * as->used_channels;
        if ((n = snd_pcm_readn(as->handle, as->bufs.ptr, count / i)) < 0) {
            goto bfio_read_error;
        }
    }    
    return n * i;

 bfio_read_error:
    switch (n) {
    case -EPIPE:
        if (as->ignore_xrun) {
            fprintf(stderr, "ALSA I/O: overflow! (read on %s)\n",
                    as->device);
            if ((err = snd_pcm_prepare(as->handle)) < 0) {
                fprintf(stderr, "ALSA I/O: Unable to prepare audio: %s.\n",
                        snd_strerror(err));
                errno = EPIPE;
                return -1;
            }
            if ((err = snd_pcm_start(as->handle)) < 0) {
                fprintf(stderr, "ALSA I/O: Could not restart audio: %s.\n",
                        snd_strerror(err));
                errno = -EPIPE;
                return -1;
            }
            goto bfio_read_restart;
        }
        /* assume and indicate buffer overflow */
        errno = EPIPE;
        break;
    case -EAGAIN:
        errno = EAGAIN;
        return -1;
    default:
        errno = 0;
        break;
    }
    fprintf(stderr, "ALSA I/O: Could not read audio: %s.\n", snd_strerror(n));
    return -1;
}

int
bfio_write(int fd,
	   const void *buf,
	   int offset,
	   int count)
{
    struct alsa_access_state *as = &fd2as[fd];
    const uint8_t *ptr;
    int n, i, err;

    if (as->isinterleaved) {
	i = as->sample_size * as->open_channels;
	if ((n = snd_pcm_writei(as->handle, &((const uint8_t *)buf)[offset],
                                count / i)) < 0)
        {
            goto bfio_write_error;
	}
    } else {
        ptr = buf;
        ptr += offset / as->used_channels;
        for (n = 0; n < as->used_channels; n++) {
            as->bufs.const_ptr[as->channel_selection[n]] = ptr;
            ptr += as->sw_period_size * as->sample_size;
        }
        i = as->sample_size * as->used_channels;
        if ((n = snd_pcm_writen(as->handle, as->bufs.ptr, count / i)) < 0) {
            goto bfio_write_error;
        }
    }
    if (as->restart) {
        as->restart = false;
        if ((err = snd_pcm_start(as->handle)) < 0) {
            fprintf(stderr, "ALSA I/O: Could not restart audio: %s.\n",
                    snd_strerror(err));
            errno = -EPIPE;
            return -1;
        }
    }
    return n * i;

 bfio_write_error:
    switch (n) {
    case -EPIPE:
        if (as->ignore_xrun) {
            fprintf(stderr, "ALSA I/O: underflow! (write on %s)\n",
                    as->device);
            if ((err = snd_pcm_prepare(as->handle)) < 0) {
                fprintf(stderr, "ALSA I/O: Unable to prepare audio: %s.\n",
                        snd_strerror(err));
                errno = EPIPE;
                return -1;
            }
            as->restart = true;
            return count;
        }
        /* assume and indicate buffer underflow */
        errno = EPIPE;
        break;
    case -EAGAIN:
        errno = EAGAIN;
        return -1;
    default:
        errno = 0;
        break;
    }
    fprintf(stderr, "ALSA I/O: Could not write audio: %s.\n", snd_strerror(n));
    return -1;
}

void
do_init(void);
void __attribute__((constructor))
do_init(void)
{
    memset(handles, 0, sizeof(handles));
    memset(n_handles, 0, sizeof(n_handles));
    memset(fd2as, 0, sizeof(fd2as));
}
