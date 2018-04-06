/*
 * (c) Copyright 2003 - 2006, 2009, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>

#include <jack/jack.h>

#include "defs.h"
#include "emalloc.h"
#define IS_BFIO_MODULE
#include "bfmod.h"
#include "inout.h"

struct jack_state {
    int n_channels;
    jack_port_t *ports[BF_MAXCHANNELS];
    char *port_name[BF_MAXCHANNELS];
    char *local_port_name[BF_MAXCHANNELS];
    char *dest_name[BF_MAXCHANNELS];
};

#define DEFAULT_CLIENTNAME "brutefir"
#define DEFAULT_JACK_CB_THREAD_PRIORITY 5

static bool_t debug;
static volatile bool_t stopped = false;
static volatile bool_t has_started = false;
static struct jack_state *handles[2][BF_MAXCHANNELS];
static int expected_priority = -1;
static void **states[2];
static int n_handles[2];
static bool_t hasio[2];
static jack_client_t *client = NULL;
static char *client_name = NULL;
static int (*process_cb)(void **_states[2],
                         int state_count[2],
                         void **bufs[2],
                         int count,
                         int event);

static void
error_callback(const char *msg)
{
    if (stopped) {
        /* we don't care about errors after we have stopped */
        return;
    }
    
    if (msg[strlen(msg)-1] ==  '\n') {
        fprintf(stderr, "JACK I/O: JACK reported an error: %s", msg);
    } else {
        fprintf(stderr, "JACK I/O: JACK reported an error: %s\n", msg);
    }
    if (has_started) {
        stopped = true;
        process_cb(states, n_handles, NULL, 0, BF_CALLBACK_EVENT_ERROR);
    }
}

static void
shutdown_callback(void *arg)
{
    fprintf(stderr, "JACK I/O: JACK daemon shut down.\n");
    stopped = true;
    process_cb(states, n_handles, NULL, 0, BF_CALLBACK_EVENT_ERROR);
}

static void
init_callback_(void)
{
    struct sched_param schp;
    int policy;

    pthread_getschedparam(pthread_self(), &policy, &schp);
    if (policy != SCHED_FIFO && policy != SCHED_RR) {
        fprintf(stderr, "JACK I/O: Warning: JACK is not running with "
                "SCHED_FIFO or SCHED_RR (realtime).\n");
    } else if (schp.sched_priority != expected_priority) {
        fprintf(stderr, "\
JACK I/O: Warning: JACK thread has priority %d, but BruteFIR expected %d.\n\
  In order to make correct realtime scheduling BruteFIR must know what\n\
  priority JACK uses. At the time of writing the JACK API does not support\n\
  getting that information so BruteFIR must be manually configured with that.\n\
  Use the \"priority\" setting in the first \"jack\" device clause in your\n\
  BruteFIR configuration file.\n",
                (int)schp.sched_priority, (int)expected_priority);
    }
}
    
static void
init_callback(void *arg)
{
    static pthread_once_t once_control = PTHREAD_ONCE_INIT;

    pthread_once(&once_control, init_callback_);
}

static void
latency_callback(jack_latency_callback_mode_t mode,
                 void *arg)
{
    const int period_size = (intptr_t)arg;
    jack_latency_range_t range;
    struct jack_state *js;
    int n, i;

    // same latency for all ports, regardless of how they are connected
    if (mode == JackPlaybackLatency) {
        // do nothing
    } else if (mode == JackCaptureLatency) {
        for (n = 0; n < n_handles[OUT]; n++) {
            js = handles[OUT][n];
            for (i = 0; i < js->n_channels; i++) {
                range.min = period_size;
                range.max = period_size;
                jack_port_set_latency_range(js->ports[i], mode, &range);
            }
        }
    }
}

static int
process_callback(jack_nframes_t n_frames,
                 void *arg)
{
    static int frames_left = 0;
    
    void *in_bufs[BF_MAXCHANNELS], *out_bufs[BF_MAXCHANNELS], **iobufs[2];
    struct jack_state *js;
    void *buffer = NULL;
    int n, i;

    iobufs[IN] = n_handles[IN] > 0 ? in_bufs : NULL;
    iobufs[OUT] = n_handles[OUT] > 0 ? out_bufs : NULL;
    FOR_IN_AND_OUT {
        for (n = 0; n < n_handles[IO]; n++) {
            js = handles[IO][n];
            for (i = 0; i < js->n_channels; i++) {
                iobufs[IO][i] = jack_port_get_buffer(js->ports[i], n_frames);
            }
        }
    }
    if (frames_left != 0) {
        process_cb(states, n_handles, NULL, 0, BF_CALLBACK_EVENT_FINISHED);
        for (n = 0; n < n_handles[OUT]; n++) {
            js = handles[OUT][n];
            for (i = 0; i < js->n_channels; i++) {
                buffer = jack_port_get_buffer(js->ports[i], n_frames);
                memset(buffer, 0, n_frames *
                       sizeof(jack_default_audio_sample_t));
            }
        }
        stopped = true;
        return -1;
    }
    frames_left = process_cb(states, n_handles, iobufs, n_frames,
                             BF_CALLBACK_EVENT_NORMAL);
    if (frames_left == -1) {
        stopped = true;
        return -1;
    }
    
    return 0;
}

static bool_t
global_init(void)
{
    jack_status_t status;
    jack_set_error_function(error_callback);
    if ((client = jack_client_open(client_name, JackNoStartServer,
                                   &status)) == NULL)
    {
        fprintf(stderr, "\
JACK I/O: Could not become JACK client (status: 0x%2.0x). Error message(s):\n",
                status);
        if ((status & JackFailure) != 0) {
            fprintf(stderr, "\
  Overall operation failed.\n");
        }
        if ((status & JackInvalidOption) != 0) {
            fprintf(stderr, "\
  Likely bug in BruteFIR: the operation contained an invalid or unsupported\n\
  option.\n");
        }
        if ((status & JackNameNotUnique) != 0) {
            fprintf(stderr, "\
  Client name \"%s\" not unique, try another name.\n", client_name);
        }
        if ((status & JackServerFailed) != 0) {
            fprintf(stderr, "\
  Unable to connect to the JACK server. Perhaps it is not running? BruteFIR\n\
  requires that a JACK server is started in advance.\n");
        }
        if ((status & JackServerError) != 0) {
            fprintf(stderr, "  Communication error with the JACK server.\n");
        }
        if ((status & JackNoSuchClient) != 0) {
            fprintf(stderr, "  Requested client does not exist.\n");
        }
        if ((status & JackLoadFailure) != 0) {
            fprintf(stderr, "  Unable to load internal client.\n");
        }
        if ((status & JackInitFailure) != 0) {
            fprintf(stderr, "  Unable initialize client.\n");
        }
        if ((status & JackShmFailure) != 0) {
            fprintf(stderr, "  Unable to access shared memory.\n");
        }
        if ((status & JackVersionError) != 0) {
            fprintf(stderr, "\
  The version of the JACK server is not compatible with the JACK client\n\
  library used by BruteFIR.\n");
        }
        return false;
    }
    jack_set_thread_init_callback(client, init_callback, NULL);
    jack_set_process_callback(client, process_callback, NULL);
    jack_on_shutdown(client, shutdown_callback, NULL);
    
    return true;
}

int
bfio_iscallback(void)
{
    return true;
}

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "JACK I/O: Parse error: " errstr);                     \
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
             struct sched_param *callback_sched_param,
             int _debug)
{
    int rate, n, ver, token;
    union bflexval lexval;
    struct jack_state *js;

    ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return NULL;
    }
    debug = !!_debug;
    
    if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
#ifdef __LITTLE_ENDIAN__
        if (sizeof(jack_default_audio_sample_t) == 4) {
            *sample_format = BF_SAMPLE_FORMAT_FLOAT_LE;
        } else if (sizeof(jack_default_audio_sample_t) == 8) {
            *sample_format = BF_SAMPLE_FORMAT_FLOAT64_LE;
        }
#endif        
#ifdef __BIG_ENDIAN__
        if (sizeof(jack_default_audio_sample_t) == 4) {
            *sample_format = BF_SAMPLE_FORMAT_FLOAT_BE;
        } else if (sizeof(jack_default_audio_sample_t) == 8) {
            *sample_format = BF_SAMPLE_FORMAT_FLOAT64_BE;
        }
#endif        
    }
    if (sizeof(jack_default_audio_sample_t) == 4) {
#ifdef __LITTLE_ENDIAN__
        if (*sample_format != BF_SAMPLE_FORMAT_FLOAT_LE) {
            fprintf(stderr, "JACK I/O: Sample format must be %s or %s.\n",
                    bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT_LE),
                    bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
            return NULL;
        }
#endif        
#ifdef __BIG_ENDIAN__
        if (*sample_format != BF_SAMPLE_FORMAT_FLOAT_BE) {
            fprintf(stderr, "JACK I/O: Sample format must be %s or %s.\n",
                    bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT_BE),
                    bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
            return NULL;
        }
#endif        
    } else if (sizeof(jack_default_audio_sample_t) == 8) {
#ifdef __LITTLE_ENDIAN__
        if (*sample_format != BF_SAMPLE_FORMAT_FLOAT64_LE) {
            fprintf(stderr, "JACK I/O: Sample format must be %s or %s.\n",
                    bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT64_LE),
                    bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
            return NULL;
        }
#endif        
#ifdef __BIG_ENDIAN__
        if (*sample_format != BF_SAMPLE_FORMAT_FLOAT64_BE) {
            fprintf(stderr, "JACK I/O: Sample format must be %s or %s.\n",
                    bf_strsampleformat(BF_SAMPLE_FORMAT_FLOAT64_BE),
                    bf_strsampleformat(BF_SAMPLE_FORMAT_AUTO));
            return NULL;
        }
#endif        
    }
    js = emalloc(sizeof(struct jack_state));
    memset(js, 0, sizeof(struct jack_state));
    js->n_channels = open_channels;
    while ((token = get_config_token(&lexval)) > 0) {
        if (token != BF_LEXVAL_FIELD) {
            fprintf(stderr, "JACK I/O: Parse error: expected field.\n");
            return NULL;
        }
        if (strcmp(lexval.field, "ports") == 0) {
            for (n = 0; n < open_channels; n++) {
                GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
                if (lexval.string[0] != '\0') {
                    js->dest_name[n] = estrdup(lexval.string);
                }
                if ((token = get_config_token(&lexval)) == BF_LEX_SLASH) {
                    GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
                    if (lexval.string[0] != '\0') {
                        js->local_port_name[n] = estrdup(lexval.string);
                    }
                    token = get_config_token(&lexval);
                }
                if (n < open_channels - 1) {
                    if (token != BF_LEX_COMMA) {
                        fprintf(stderr, "JACK I/O: Parse error: "
                                "expected comma (,).\n");
                        return NULL;
                    }
                } else {
                    if (token != BF_LEX_EOS) {
                        fprintf(stderr, "JACK I/O: Parse error: "
                                "expected end of statement (;).\n");
                        return NULL;
                    }
                }
            }
        } else if (strcmp(lexval.field, "clientname") == 0) {
            GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
            if (client != NULL &&
                strcmp(lexval.string, client_name) != 0)
            {
                fprintf(stderr, "JACK I/O: clientname setting is global and "
                        "must be set in the first jack device.\n");
                return NULL;
            }
            if (client_name == NULL) {
                client_name = estrdup(lexval.string);
            }
            GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
        } else if (strcmp(lexval.field, "priority") == 0) {            
            GET_TOKEN(BF_LEXVAL_REAL, "expected integer.\n");
            if (client != NULL && expected_priority != (int)lexval.real) {
                fprintf(stderr, "JACK I/O: priority setting is global and "
                        "must be set in the first jack device.\n");
                return NULL;
            }
            expected_priority = (int)lexval.real;
            GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
        }
    }
    hasio[io] = true;

    if (expected_priority < 0) {
        expected_priority = DEFAULT_JACK_CB_THREAD_PRIORITY;
    }
    if (client == NULL) {
        if (client_name == NULL) {
            client_name = estrdup(DEFAULT_CLIENTNAME);
        }
        if (!global_init()) {
            return NULL;
        }
    }

    memset(callback_sched_param, 0, sizeof(*callback_sched_param));
    callback_sched_param->sched_priority = expected_priority;
    *callback_sched_policy = SCHED_FIFO;

    if ((rate = (int)jack_get_sample_rate(client)) == 0) {
        *uses_sample_clock = 0;
    } else {
        *uses_sample_clock = 1;
    }
    if (rate != 0 && rate != sample_rate) {
        fprintf(stderr, "JACK I/O: JACK sample rate is %d, BruteFIR is %d, "
                "they must be same.\n", rate, sample_rate);
        return NULL;
    }
    return (void *)js;
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
          int (*bf_process_callback)(void **callback_states[2],
                                     int callback_state_count[2],
                                     void **buffers[2],
                                     int frame_count,
                                     int event))
{
    static void *_states[2][BF_MAXCHANNELS];
    static int io_idx[2] = { 0, 0 };
    
    char *name, _name[128], longname[1024];
    struct jack_state *js;
    jack_port_t *port;
    int n;

    process_cb = bf_process_callback;    
    *device_period_size = jack_get_buffer_size(client);
    *isinterleaved = false;
    js = (struct jack_state *)params;

    if (used_channels != open_channels) {
        fprintf(stderr, "JACK I/O: Open channels must be equal to used "
                "channels for this I/O module.\n");
        return -1;
    }
    
    for (n = 0; n < used_channels; n++) {
        if (js->dest_name[n] != NULL) {
            if ((port = jack_port_by_name(client, js->dest_name[n])) == NULL) {
                fprintf(stderr, "JACK I/O: Failed to open JACK port \"%s\".\n",
                        js->dest_name[n]);
                return -1;
            }
            if ((io == IN && (jack_port_flags(port) & JackPortIsOutput) == 0) ||
                (io == OUT && (jack_port_flags(port) & JackPortIsInput) == 0))
            {
                fprintf(stderr, "JACK I/O: JACK port \"%s\" is not an %s.\n",
                        js->dest_name[n], io == IN ? "Output" : "Input");
                return -1;
            }
        }

        if (js->local_port_name[n] != NULL) {
            name = js->local_port_name[n];
        } else {
            name = _name;
            sprintf(name, "%s-%d", io == IN ? "input" : "output", io_idx[io]++);
        }

        port = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE,
                                  (io == IN ? JackPortIsInput :
                                   JackPortIsOutput), 0);
        if (port == NULL) {
            fprintf(stderr, "JACK I/O: Failed to open new JACK port.\n");
            return -1;
        }
//        if (io == OUT) {
//            jack_port_set_latency(port, period_size);
//        }
        js->ports[n] = port;
        snprintf(longname, sizeof(longname), "%s:%s", client_name, name);
        longname[sizeof(longname) - 1] = '\0';
        js->port_name[n] = estrdup(longname);
    }

    _states[io][n_handles[io]] = callback_state;
    handles[io][n_handles[io]++] = js;

    states[IN] = n_handles[IN] > 0 ? _states[IN] : NULL;
    states[OUT] = n_handles[OUT] > 0 ? _states[OUT] : NULL;    

    if (io == OUT && hasio[IN]) {
        jack_set_latency_callback(client, latency_callback,
                                  (void *)(intptr_t)period_size);
    }

    return 0;
}

int
bfio_synch_start(void)
{
    struct jack_state *js;
    sigset_t signals;
    int n, i;

    if (has_started) {
        return 0;
    }
    if (client == NULL) {
        fprintf(stderr, "JACK I/O: client is NULL\n");
        return -1;
    }
    has_started = true;
    /*
     * jack_activate() will start a new pthread. We block all signals before
     * calling to make sure that we get all signals to our thread. This is a
     * bit ugly, since it assumes that the JACK library does not mess up the
     * signal handlers later.
     */
    sigfillset(&signals);
    pthread_sigmask(SIG_BLOCK, &signals, NULL);
    n = jack_activate(client);
    pthread_sigmask(SIG_UNBLOCK, &signals, NULL);
    if (n != 0) {
        fprintf(stderr, "JACK I/O: Could not activate local JACK client.\n");
        has_started = false;
        return -1;
    }
    for (n = 0; n < n_handles[IN]; n++) {
        js = handles[IN][n];
        for (i = 0; i < js->n_channels; i++) {
            if (js->dest_name[i] == NULL) {
                continue;
            }
            if (jack_connect(client, js->dest_name[i], js->port_name[i]) != 0) {
                fprintf(stderr, "JACK I/O: Could not connect local port \"%s\" "
                        "to \"%s\".\n", js->port_name[i], js->dest_name[i]);
                has_started = false;
                return -1;
            }
        }
    }
    for (n = 0; n < n_handles[OUT]; n++) {
        js = handles[OUT][n];
        for (i = 0; i < js->n_channels; i++) {
            if (js->dest_name[i] == NULL) {
                continue;
            }
            if (jack_connect(client, js->port_name[i], js->dest_name[i]) != 0) {
                fprintf(stderr, "JACK I/O: Could not connect local port \"%s\" "
                        "to \"%s\".\n", js->port_name[i], js->dest_name[i]);
                has_started = false;
                return -1;
            }
        }
    }
    return 0;
}

void
bfio_synch_stop(void)
{
    if (!stopped) {
        stopped = true;
        jack_client_close(client);
    }
}    

void
do_init(void);
void __attribute__((constructor))
do_init(void)
{
    memset(hasio, 0, sizeof(hasio));
    memset(handles, 0, sizeof(handles));
    memset(n_handles, 0, sizeof(n_handles));
}
