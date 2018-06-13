/*
 * (c) Copyright 2004 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/soundcard.h>

#include "defs.h"
#include "log2.h"
#include "bit.h"
#include "emalloc.h"
#define IS_BFIO_MODULE
#include "bfmod.h"
#include "inout.h"

#define ERRORSIZE 1024

struct oss_device_info {
    char *device;
    bool_t dir[2];
    bool_t trigger;
    int sample_format;
    int sample_rate;
    int open_channels;
    int period_size;
    int fd;
};

struct settings {
    char *device;
};

static struct oss_device_info *devices[2 * BF_MAXCHANNELS];
static int n_devices = 0;
static bool_t debug = false;

static bool_t
set_params(int fd,
	   int sample_format,
	   int sample_rate,
	   int open_channels,
	   int period_size,
	   int *hardware_period_size,
	   char errstr[])
{
    int format, n;

    /* Set the fragment size */
    n = period_size * open_channels * bf_sampleformat_size(sample_format);
    n = (0x7FFF << 16) | n;
    /* we check the actual result later (n does not get the true value) */
    if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &n) == -1) {
        sprintf(errstr, "  Could not set fragment size: %s.\n",
                strerror(errno));
        return false;
    }
    
    /* Set sample format */
    switch (sample_format) {
    case BF_SAMPLE_FORMAT_S8:
        format = AFMT_S8;
	break;
    case BF_SAMPLE_FORMAT_S16_LE:
        format = AFMT_S16_LE;
	break;
    case BF_SAMPLE_FORMAT_S16_BE:
        format = AFMT_S16_BE;
	break;
#ifdef AFMT_S24_LE
    case BF_SAMPLE_FORMAT_S24_LE:
        format = AFMT_S24_LE;
	break;
    case BF_SAMPLE_FORMAT_S24_BE:
        format = AFMT_S24_BE;
	break;
#endif        
#ifdef AFMT_S32_LE
    case BF_SAMPLE_FORMAT_S24_4LE:
    case BF_SAMPLE_FORMAT_S32_LE:
	format = AFMT_S32_LE;
	break;
    case BF_SAMPLE_FORMAT_S24_4BE:
    case BF_SAMPLE_FORMAT_S32_BE:
	format = AFMT_S32_BE;
	break;
#endif        
    default:
	sprintf(errstr, "  Unsupported sample format.\n");
	return false;
    }
    n = format;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &n) == -1) {
        sprintf(errstr, "  Could not set sample format: %s.\n",
                strerror(errno));
        return false;
    }
    if (n != format) {
        sprintf(errstr, "  Sample format %s is not supported by the device.\n",
                bf_strsampleformat(sample_format));
        return false;
    }

    /* Set channel count */
    n = open_channels;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &n) == -1) {
        sprintf(errstr, "  Could not set channel count: %s.\n",
                strerror(errno));
        return false;
    }
    if (n != open_channels) {
        sprintf(errstr, "  Failed to open %d interleaved channels, device "
                "suggested %d channels instead.\n", open_channels, n);
        return false;
    }

    /* Set sample rate */
    n = sample_rate;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &n) == -1) {
        sprintf(errstr, "  Could not set sample rate: %s.\n", strerror(errno));
        return false;
    }
    /* accept a minor variation in sample rate */
    if (n != sample_rate && !((int)((double)sample_rate * 0.99) < n &&
                              (int)((double)sample_rate / 0.99) > n))
    {
        sprintf(errstr, "  Failed to set sample rate to %d Hz, device "
                "suggested %d Hz instead.\n", sample_rate, n);
        return false;
    }

    /* Get the fragment size */
    if (ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &n) == -1) {
        sprintf(errstr, "  Could not get fragment size: %s.\n",
                strerror(errno));
        return false;
    }
    *hardware_period_size = n;
    
    return true;
}

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "OSS I/O: Parse error: " errstr);                      \
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
    struct settings *settings;
    union bflexval lexval;
    int n, token, ver;

    ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return NULL;
    }
    debug = !!_debug;
    
    *uses_sample_clock = 1;
    
    settings = malloc(sizeof(struct settings));
    memset(settings, 0, sizeof(struct settings));
    while ((token = get_config_token(&lexval)) > 0) {
        if (token != BF_LEXVAL_FIELD) {
            fprintf(stderr, "OSS I/O: Parse error: expected field.\n");
            return NULL;
        }
        if (strcmp(lexval.field, "device") == 0) {
            if (settings->device != NULL) {
                fprintf(stderr, "OSS I/O: Parse error: device already set.\n");
                return NULL;
            }
            GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
            settings->device = estrdup(lexval.string);
        } else {
            fprintf(stderr, "OSS I/O: Parse error: unknown field.\n");
            return NULL;
        }
        GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
    }    
    if (settings->device == NULL) {
        fprintf(stderr, "OSS I/O: Parse error: device not set.\n");
        return NULL;
    }
    if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
        fprintf(stderr, "OSS I/O: No support for AUTO sample format.\n");
        return NULL;
    }
    for (n = 0; n < n_devices; n++) {
        if (strcmp(devices[n]->device, settings->device) == 0) {
            if (devices[n]->dir[io]) {
                fprintf(stderr, "OSS I/O: Device \"%s\" already used for "
                        "audio %s.\n", settings->device,
                        io == BF_IN ? "input" : "output");
                return NULL;
            }
            devices[n]->dir[io] = true;
        }
    }
    if (n == n_devices) {
        devices[n] = emalloc(sizeof(struct oss_device_info));
        memset(devices[n], 0, sizeof(struct oss_device_info));
        devices[n]->device = estrdup(settings->device);
        devices[n]->dir[io] = true;
        devices[n]->fd = -1;
        n_devices++;
    }
    
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
    struct oss_device_info *devinfo;
    struct settings *settings;
    char errstr[ERRORSIZE];
    int n, fd, enable_bits;
    
    settings = (struct settings *)params;
    *isinterleaved = 1;
    devinfo = NULL;
    for (n = 0; n < n_devices; n++) {
        if (strcmp(settings->device, devices[n]->device) == 0) {
            devinfo = devices[n];
            break;
        }
    }
    if (devinfo == NULL) {
        fprintf(stderr, "OSS I/O: Bug: device not found.\n");
        return -1;
    }
    if (devinfo->fd != -1) {
        /* already opened, check that the parameters matches */
        if (sample_format != devinfo->sample_format) {
            fprintf(stderr, "OSS I/O: Sample formats for input and output "
                    "on device \"%s\" do not match.\n", devinfo->device);
            return -1;
        }
        if (sample_rate != devinfo->sample_rate) {
            fprintf(stderr, "OSS I/O: Sample rate for input and output "
                    "on device \"%s\" do not match.\n", devinfo->device);
            return -1;
        }
        if (open_channels != devinfo->open_channels) {
            fprintf(stderr, "OSS I/O: Channel amount for input and output "
                    "on device \"%s\" do not match.\n", devinfo->device);
            return -1;
        }
        *device_period_size = devinfo->period_size;
        return devinfo->fd;
    }
    if (devinfo->dir[BF_IN] && devinfo->dir[BF_OUT]) {
        /* open device in full duplex */
        if ((fd = open(devinfo->device, O_RDWR)) == -1) {
            fprintf(stderr, "OSS I/O: Could not open device \"%s\" in full "
                    "duplex mode: %s.\n", devinfo->device, strerror(errno));
            return -1;
        }
        /* On *BSD, full duplex is enabled per default, which leads to that
           this ioctl() always fails with EINVAL, so we ignore that. */
        if (ioctl(fd, SNDCTL_DSP_SETDUPLEX, 0) == -1 && errno != EINVAL) {
            fprintf(stderr, "OSS I/O: Could not set device \"%s\" to full "
                    "duplex mode: %s.\n", devinfo->device, strerror(errno));
            return -1;
        }
        enable_bits = 0;
    } else {
        /* open device in half duplex */
        if ((fd = open(devinfo->device, devinfo->dir[BF_IN] ?
                       O_RDONLY : O_WRONLY)) == -1)
        {
            fprintf(stderr, "OSS I/O: Could not open device \"%s\" for audio "
                    "%s: %s.\n", devinfo->device,
                    devinfo->dir[BF_IN] ? "input" : "output", strerror(errno));
            return -1;
        }
        enable_bits = devinfo->dir[BF_IN] ?
            ~PCM_ENABLE_INPUT : ~PCM_ENABLE_OUTPUT;
    }
    if (ioctl(fd, SNDCTL_DSP_GETCAPS, &n) == -1) {
        fprintf(stderr, "OSS I/O: Could not get device \"%s\" "
                "capabilities: %s.\n", devinfo->device, strerror(errno));
        return -1;
    }
    if (devinfo->dir[BF_IN] && devinfo->dir[BF_OUT] &&
        (n & DSP_CAP_DUPLEX) == 0)
    {
        fprintf(stderr, "OSS I/O: Device \"%s\" does not support full "
                "duplex.\n", devinfo->device);
        return -1;
    }
    /* We use the trigger if it exists. It will work without triggering though
       (at least on tested platform/card) */
    if ((n & DSP_CAP_TRIGGER) != 0) {
        devinfo->trigger = true;
        if (ioctl(fd, SNDCTL_DSP_SETTRIGGER, &enable_bits) == -1) {
            fprintf(stderr, "OSS I/O: Could not set enable bits for device "
                    "\"%s\": %s.\n", devinfo->device, strerror(errno));
            return -1;
        }
    }
    if (!set_params(fd, sample_format, sample_rate, open_channels,
		    period_size, device_period_size, errstr))
    {
	fprintf(stderr, "OSS I/O: Could not set audio %s parameters for "
                "\"%s\":\n%s",
		io == BF_IN ? "input" : "output", devinfo->device, errstr);
        close(fd);
	return -1;
    }
    devinfo->fd = fd;
    devinfo->sample_format = sample_format;
    devinfo->sample_rate = sample_rate;
    devinfo->open_channels = open_channels;
    devinfo->period_size = *device_period_size;
    efree(settings->device);
    efree(settings);
    return fd;
}

int
bfio_synch_start(void)
{
    int n, enable_bits;

    for (n = 0; n < n_devices; n++) {
        if (!devices[n]->trigger) {
            continue;
        }
        enable_bits = 0;
        if (devices[n]->dir[BF_IN]) {
            enable_bits |= PCM_ENABLE_INPUT;
        }
        if (devices[n]->dir[BF_OUT]) {
            enable_bits |= PCM_ENABLE_OUTPUT;
        }
        if (ioctl(devices[n]->fd, SNDCTL_DSP_SETTRIGGER,
                  &enable_bits) == -1)
        {
            fprintf(stderr, "OSS I/O: Could not trigger device "
                    "\"%s\": %s.\n", devices[n]->device, strerror(errno));
            return -1;
        }
    }
    return 0;
}

void
bfio_synch_stop(void)
{
    int n;

    for (n = 0; n < n_devices; n++) {
        close(devices[n]->fd);
    }
}

int
bfio_read(int fd,
	  void *buf,
	  int offset,
	  int count)
{
    audio_buf_info info;

    if (ioctl(fd, SNDCTL_DSP_GETISPACE, &info) == -1) {
        fprintf(stderr, "OSS I/O: Could not get ispace info: %s.\n",
                strerror(errno));
        return -1;
    }
    if (info.bytes == 0) {
        errno = EAGAIN;
        return -1;
    }
    if (count > info.bytes) {
        count = info.bytes;
    }
    count = read(fd, &((uint8_t *)buf)[offset], count);
    return count;
}

int
bfio_write(int fd,
	   const void *buf,
	   int offset,
	   int count)
{
    audio_buf_info info;
    
    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == -1) {
        fprintf(stderr, "OSS I/O: Could not get ospace info: %s.\n",
                strerror(errno));
        return -1;
    }
    if (info.bytes == 0) {
        errno = EAGAIN;
        return -1;
    }
    if (count > info.bytes) {
        count = info.bytes;
    }
    count = write(fd, &((const uint8_t *)buf)[offset], count);
    return count;
}
