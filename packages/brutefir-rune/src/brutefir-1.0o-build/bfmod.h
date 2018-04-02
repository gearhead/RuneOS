/*
 * (c) Copyright 2001 - 2006 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _BFMOD_H_
#define _BFMOD_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sched.h>

#define BF_VERSION_MAJOR 2
#define BF_VERSION_MINOR 0
    
/* limits */
#define BF_MAXCHANNELS 256
#define BF_MAXFILTERS 256
#define BF_MAXMODULES 256
#define BF_MAXOBJECTNAME 128
#define BF_MAXCOEFFPARTS 128
#define BF_MAXPROCESSES 64

#define BF_IN 0
#define BF_OUT 1

#define BF_SAMPLE_FORMAT_MIN BF_SAMPLE_FORMAT_S8
#define BF_SAMPLE_FORMAT_S8 1
#define BF_SAMPLE_FORMAT_S16_LE 2
#define BF_SAMPLE_FORMAT_S16_BE 3
#define BF_SAMPLE_FORMAT_S16_4LE 4 // unused since 1.0m
#define BF_SAMPLE_FORMAT_S16_4BE 5 // unused since 1.0m
#define BF_SAMPLE_FORMAT_S24_LE 6
#define BF_SAMPLE_FORMAT_S24_BE 7
#define BF_SAMPLE_FORMAT_S24_4LE 8
#define BF_SAMPLE_FORMAT_S24_4BE 9
#define BF_SAMPLE_FORMAT_S32_LE 10
#define BF_SAMPLE_FORMAT_S32_BE 11
#define BF_SAMPLE_FORMAT_FLOAT_LE 12
#define BF_SAMPLE_FORMAT_FLOAT_BE 13
#define BF_SAMPLE_FORMAT_FLOAT64_LE 14
#define BF_SAMPLE_FORMAT_FLOAT64_BE 15

#define BF_SAMPLE_FORMAT_MAX BF_SAMPLE_FORMAT_FLOAT64_BE

/* macro sample formats */    
#define BF_SAMPLE_FORMAT_S16_NE 16
#define BF_SAMPLE_FORMAT_S16_4NE 17 // unused since 1.0m
#define BF_SAMPLE_FORMAT_S24_NE 18
#define BF_SAMPLE_FORMAT_S24_4NE 19
#define BF_SAMPLE_FORMAT_S32_NE 20
#define BF_SAMPLE_FORMAT_FLOAT_NE 21
#define BF_SAMPLE_FORMAT_FLOAT64_NE 22
#define BF_SAMPLE_FORMAT_AUTO 23

#define BF_SAMPLE_FORMAT_MACRO_MAX 23    

/* exit values */
#define BF_EXIT_OK 0
#define BF_EXIT_OTHER 1
#define BF_EXIT_INVALID_CONFIG 2
#define BF_EXIT_NO_MEMORY 3
#define BF_EXIT_INVALID_INPUT 4
#define BF_EXIT_BUFFER_UNDERFLOW 5

/* callback events */
#define BF_CALLBACK_EVENT_NORMAL 0
#define BF_CALLBACK_EVENT_ERROR 1
#define BF_CALLBACK_EVENT_LAST_INPUT 2
#define BF_CALLBACK_EVENT_FINISHED 3    
    
#define BF_LEX_EOS     1 /* end of statement (;) */
#define BF_LEX_LBRACE  2 /* { */
#define BF_LEX_RBRACE  3 /* } */
#define BF_LEX_COMMA   4 /* , */
#define BF_LEX_SLASH   5 /* / */

#define BF_LEXVAL_REAL    100
#define BF_LEXVAL_BOOLEAN 101
#define BF_LEXVAL_STRING  102
#define BF_LEXVAL_FIELD   103

#define BF_SAMPLE_SLOTS 100
#define BF_UNDEFINED_SUBDELAY (-BF_SAMPLE_SLOTS)
    
union bflexval {
    double real;
    int boolean;
    char *string;
    char *field;
};

struct bfoverflow {
    unsigned int n_overflows;
    int32_t intlargest;
    double largest;
    double max;
};

struct bfcoeff {
    int is_shared;
    char name[BF_MAXOBJECTNAME];
    int intname;
    int n_blocks;
};

struct bfchannel {
    char name[BF_MAXOBJECTNAME];
    int intname;
};

struct bffilter {
    char name[BF_MAXOBJECTNAME];
    int intname;
    int crossfade;
    int n_channels[2];
    int *channels[2];
    int n_filters[2];
    int *filters[2];
};

struct bffilter_control {
    int coeff;
    int delayblocks;
    double scale[2][BF_MAXCHANNELS];
    double fscale[BF_MAXFILTERS];
};

struct bfaccess {
    volatile struct bffilter_control *fctrl;
    volatile struct bfoverflow *overflow;
    int realsize;
    void ***coeffs_data;
    void (*control_mutex)(int lock);
    void (*reset_peak)(void);
    void (*exit)(int bf_exit_code);
/*
 * Mute/unmute the given input/output channel. If the channel index is invalid,
 * nothing happens.
 */
    void (*toggle_mute)(int io,
			int channel);  
    int (*ismuted)(int io,
		   int channel);
    
/*
 * Change delay of the given channel. If the delay or channel is out of range,
 * -1 is returned, else 0.
 */
    int (*set_delay)(int io,
		     int channel,
		     int delay);
    int (*get_delay)(int io,
		     int channel);

    double (*realtime_index)(void);

    const char **(*bfio_names)(int io,
			       int *n_names);
    void (*bfio_range)(int io,
		       int modindex,
		       int range[2]);
    int (*bfio_command)(int io,
                        int modindex,
                        const char params[],
                        char **error);
    const char **(*bflogic_names)(int *n_names);
    int (*bflogic_command)(int modindex,
                           const char params[],
                           char **error);

    void (*convolver_coeffs2cbuf)(void *src,
                                  void *dest);

    void *(*convolver_fftplan)(int order,
                               int invert,
                               int inplace);

    int (*set_subdelay)(int io,
                        int channel,
                        int subdelay);
    int (*get_subdelay)(int io,
                        int channel);
};

struct bfevents {
#define BF_FDEVENT_PEAK 0x1
#define BF_FDEVENT_INITIALISED 0x2    
    unsigned int fdevents;
    void (*peak)(void);
    void (*initialised)(void);
    void (*block_start)(struct bfaccess *bfaccess,
                        unsigned int block_index,
                        struct timeval *current_time);
    void (*input_timed)(void *buf,
			int channel);
    void (*input_freqd)(void *buf,
			int channel);
    void (*coeff_final)(int filter,
                        int *coeff);
    void (*pre_convolve)(void *buf,
			 int filter);
    void (*post_convolve)(void *buf,
			  int filter);
    void (*output_freqd)(void *buf,
			 int channel);
    void (*output_timed)(void *buf,
			 int channel);
};

#define BF_FUN_BFIO_ISCALLBACK  "bfio_iscallback"
#define BF_FUN_BFIO_PREINIT     "bfio_preinit"
#define BF_FUN_BFIO_COMMAND     "bfio_command"
#define BF_FUN_BFIO_INIT        "bfio_init"
#define BF_FUN_BFIO_READ        "bfio_read"
#define BF_FUN_BFIO_WRITE       "bfio_write"
#define BF_FUN_BFIO_SYNCH_START "bfio_synch_start"
#define BF_FUN_BFIO_SYNCH_STOP  "bfio_synch_stop"
#define BF_FUN_BFIO_START       "bfio_start"
#define BF_FUN_BFIO_STOP        "bfio_stop"
#define BF_FUN_BFIO_MESSAGE     "bfio_message"

struct bfio_module {
    void *handle;
    int iscallback;
    void *(*preinit)(int *version_minor,
                     int *version_major,
                     int (*get_config_token)(union bflexval *lexval),
                     int io,
                     int *sample_format,
                     int sample_rate,
                     int open_channels,
                     int *uses_sample_clock,
                     int *callback_sched_policy,
                     struct sched_param *callback_sched,
                     int debug);
    int (*init)(void *params,
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
                                        int event));
    int (*command)(int fd,
                   const char params[]);
    /* see errno handling in dai_input in order to provide correct errnos */
    int (*read)(int fd,
		void *buf,
		int offset,
		int count);
    int (*write)(int fd,
		 const void *buf,
		 int offset,
		 int count);
    int (*synch_start)(void);
    void (*synch_stop)(void);
    int (*start)(int io);
    void (*stop)(int io);
    const char *(*message)(void);
};

#define BF_FUN_BFLOGIC_PREINIT    "bflogic_preinit"
#define BF_FUN_BFLOGIC_INIT       "bflogic_init"
#define BF_FUN_BFLOGIC_COMMAND    "bflogic_command"
#define BF_FUN_BFLOGIC_MESSAGE    "bflogic_message"

struct bflogic_module {
    void *handle;
#define BF_FORK_DONT_FORK   0
#define BF_FORK_PRIO_MAX    1
#define BF_FORK_PRIO_FILTER 2
#define BF_FORK_PRIO_OTHER  3
    int fork_mode;
    int event_pipe[2];
    struct bfevents bfevents;
    int (*preinit)(int *version_minor,
                   int *version_major,
                   int (*get_config_token)(union bflexval *lexval),
                   int sample_rate,
                   int block_length,
                   int n_maxblocks,
                   int n_coeffs,
                   const struct bfcoeff coeffs[],
                   const int n_channels[2],
                   const struct bfchannel * const *channels,
                   int n_filters,
                   const struct bffilter filters[],
                   struct bfevents *bfevents,
                   int *fork_mode,
                   int debug);
    int (*init)(struct bfaccess *bfaccess,
		int sample_rate,
		int block_length,
		int n_maxblocks,
		int n_coeffs,
		const struct bfcoeff coeffs[],
		const int n_channels[2],
		const struct bfchannel *channels[2],
		int n_filters,
		const struct bffilter filters[],
		int event_fd,
                int synch_fd);
    int (*command)(const char params[]);
    const char *(*message)(void);
};

static inline int
bf_sampleformat_size(int format)
{
    switch (format) {
    case BF_SAMPLE_FORMAT_S8:
	return 1;
    case BF_SAMPLE_FORMAT_S16_LE:
	return 2;
    case BF_SAMPLE_FORMAT_S16_BE:
	return 2;
    case BF_SAMPLE_FORMAT_S16_4LE:
	return 4;
    case BF_SAMPLE_FORMAT_S16_4BE:
	return 4;
    case BF_SAMPLE_FORMAT_S24_LE:
	return 3;
    case BF_SAMPLE_FORMAT_S24_BE:
	return 3;
    case BF_SAMPLE_FORMAT_S24_4LE:
	return 4;
    case BF_SAMPLE_FORMAT_S24_4BE:
	return 4;
    case BF_SAMPLE_FORMAT_S32_LE:
	return 4;
    case BF_SAMPLE_FORMAT_S32_BE:
	return 4;
    case BF_SAMPLE_FORMAT_FLOAT_LE:
	return 4;
    case BF_SAMPLE_FORMAT_FLOAT_BE:
	return 4;
    case BF_SAMPLE_FORMAT_FLOAT64_LE:
	return 8;
    case BF_SAMPLE_FORMAT_FLOAT64_BE:
	return 8;
    default:
	return 0;
    }
}

static inline const char *
bf_strsampleformat(int format)
{
    switch (format) {
    case BF_SAMPLE_FORMAT_S8:
	return "S8";
    case BF_SAMPLE_FORMAT_S16_LE:
	return "S16_LE";
    case BF_SAMPLE_FORMAT_S16_BE:
	return "S16_BE";
    case BF_SAMPLE_FORMAT_S16_NE:
	return "S16_NE";
    case BF_SAMPLE_FORMAT_S16_4LE:
	return "S16_4LE";
    case BF_SAMPLE_FORMAT_S16_4BE:
	return "S16_4BE";
    case BF_SAMPLE_FORMAT_S16_4NE:
	return "S16_4NE";
    case BF_SAMPLE_FORMAT_S24_LE:
	return "S24_LE";
    case BF_SAMPLE_FORMAT_S24_BE:
	return "S24_BE";
    case BF_SAMPLE_FORMAT_S24_NE:
	return "S24_NE";
    case BF_SAMPLE_FORMAT_S24_4LE:
	return "S24_4LE";
    case BF_SAMPLE_FORMAT_S24_4BE:
	return "S24_4BE";
    case BF_SAMPLE_FORMAT_S24_4NE:
	return "S24_4NE";
    case BF_SAMPLE_FORMAT_S32_LE:
	return "S32_LE";
    case BF_SAMPLE_FORMAT_S32_BE:
	return "S32_BE";
    case BF_SAMPLE_FORMAT_S32_NE:
	return "S32_NE";
    case BF_SAMPLE_FORMAT_FLOAT_LE:
	return "FLOAT_LE";
    case BF_SAMPLE_FORMAT_FLOAT_BE:
	return "FLOAT_BE";
    case BF_SAMPLE_FORMAT_FLOAT_NE:
	return "FLOAT_NE";
    case BF_SAMPLE_FORMAT_FLOAT64_LE:
	return "FLOAT64_LE";
    case BF_SAMPLE_FORMAT_FLOAT64_BE:
	return "FLOAT64_BE";
    case BF_SAMPLE_FORMAT_FLOAT64_NE:
	return "FLOAT64_NE";
    case BF_SAMPLE_FORMAT_AUTO:
        return "AUTO";
    default:
	return "##unknown sample format##";
    }
}

#ifdef IS_BFIO_MODULE
/* prototypes of functions the module implements */
int
bfio_iscallback(void);

void *
bfio_preinit(int *version_minor,
             int *version_major,
             int (*get_config_token)(union bflexval *lexval),
             int io,
             int *sample_format,
             int sample_rate,
             int open_channels,
             int *uses_sample_clock,
             int *callback_sched_policy,
             struct sched_param *callback_sched,
             int debug);

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
                                  int event));

int
bfio_cb_init(void *params);

int
bfio_command(int fd,
             const char params[]);

int
bfio_read(int fd,
          void *buf,
          int offset,
          int count);

int
bfio_write(int fd,
           const void *buf,
           int offset,
           int count);

int
bfio_synch_start(void);

void
bfio_synch_stop(void);

int
bfio_start(int io);

void
bfio_stop(int io);

const char *
bfio_message(void);

#endif

#ifdef IS_BFLOGIC_MODULE
/* prototypes of functions the module implements */

int
bflogic_preinit(int *version_minor,
                int *version_major,
                int (*get_config_token)(union bflexval *lexval),
                int sample_rate,
                int block_length,
                int n_maxblocks,
                int n_coeffs,
                const struct bfcoeff coeffs[],
                const int n_channels[2],
                const struct bfchannel *channels[2],
                int n_filters,
                const struct bffilter filters[],
                struct bfevents *bfevents,
                int *fork_mode,
                int debug);

int
bflogic_init(struct bfaccess *bfaccess,
             int sample_rate,
             int block_length,
             int n_maxblocks,
             int n_coeffs,
             const struct bfcoeff coeffs[],
             const int n_channels[2],
             const struct bfchannel *channels[2],
             int n_filters,
             const struct bffilter filters[],
             int event_fd,
             int synch_fd);

int
bflogic_command(const char params[]);

const char *
bflogic_message(void);

#endif

#ifdef __cplusplus
}
#endif

#endif
