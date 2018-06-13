/*
 * (c) Copyright 2002 - 2005 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/select.h>

#include <fftw3.h>

#define IS_BFLOGIC_MODULE
#include "bfmod.h"
#include "emalloc.h"
#include "shmalloc.h"
#include "defs.h"
#include "log2.h"
#include "bit.h"
#include "fdrw.h"
#include "timermacros.h"

#define MAX_EQUALISERS 64
#define MAX_BANDS 128
#define MSGSIZE (MAX_BANDS * 20)

#define CMD_CHANGE_MAGNITUDE 1
#define CMD_CHANGE_PHASE 2
#define CMD_GET_INFO 3

struct realtime_eq {
    void *ifftplan;
    int band_count;
    int taps;
    volatile int coeff[2];
    volatile int active_coeff;
    volatile bool_t not_changed;
    double *freq;
    double *mag;
    double *phase;
};


static struct realtime_eq *equalisers;
static int n_equalisers = 0;
static char msg[MSGSIZE];
static struct bfaccess *bfaccess;
static int sample_rate;
static int block_length;
static int n_maxblocks;
static int n_coeffs;
static int n_filters;
static const struct bfcoeff *coeffs;
static char *debug_dump_filter_path = NULL;
static int cmdpipe[2], cmdpipe_reply[2];
static bool_t debug = false;
static void *rbuf;

#define real_t float
#define REALSIZE 4
#define COSINE_INT_NAME cosine_int_f
#define RENDER_EQUALISER_NAME render_equaliser_f
#include "rendereq.h"
#undef COSINE_INT_NAME
#undef RENDER_EQUALISER_NAME
#undef REALSIZE
#undef real_t

#define real_t double
#define REALSIZE 8
#define COSINE_INT_NAME cosine_int_d
#define RENDER_EQUALISER_NAME render_equaliser_d
#include "rendereq.h"
#undef COSINE_INT_NAME
#undef RENDER_EQUALISER_NAME
#undef REALSIZE
#undef real_t

static char *
strtrim(char s[])
{
    char *p;
    
    while (*s == ' ' || *s == '\t') {
	s++;
    }
    if (*s == '\0') {
	return s;
    }
    p = s + strlen(s) - 1;
    while ((*p == ' ' || *p == '\t') && p != s) {
	p--;
    }
    *(p + 1) = '\0';
    return s;
}

static void
coeff_final(int filter,
            int *coeff)
{
    int n, c[2], active;

    for (n = 0; n < n_equalisers; n++) {
        c[0] = equalisers[n].coeff[0];
        c[1] = equalisers[n].coeff[1];
        active = c[equalisers[n].active_coeff];
        if (*coeff == c[0] || *coeff == c[1]) {
            *coeff = active;
            equalisers[n].not_changed = false;
        }
    }
}

static bool_t
finalise_equaliser(struct realtime_eq *eq,
                   double mfreq[],
                   double mag[],
                   int n_mag,
                   double pfreq[],
                   double phase[],
                   int n_phase,
                   double bands[],
                   int n_bands)
{
    int n, i, band_count;

    band_count = n_bands + 2;
    eq->freq = emalloc(band_count * sizeof(double));
    eq->mag = emalloc(band_count * sizeof(double));
    eq->phase = emalloc(band_count * sizeof(double));
    eq->freq[0] = 0.0;
    for (n = 0; n < n_bands; n++) {
        eq->freq[1+n] = bands[n];
    }
    eq->freq[1+n] = (double)sample_rate / 2.0;                
    
    memset(eq->mag, 0, band_count * sizeof(double));
    for (n = 0, i = 0; n < n_mag; n++) {
        while (mfreq[n] > eq->freq[i]) {
            i++;
        }
        if (mfreq[n] != eq->freq[i]) {
            fprintf(stderr, "EQ: %.1f Hz is not a band frequency, "
                    "use %.1f instead.\n", mfreq[n], eq->freq[i]);
            return false;
        }
        eq->mag[i++] = mag[n];
    }
    eq->mag[0] = eq->mag[1];
    eq->mag[band_count - 1] = eq->mag[band_count - 2];
    
    memset(eq->phase, 0, band_count * sizeof(double));
    for (n = 0, i = 0; n < n_phase; n++) {
        while (pfreq[n] > eq->freq[i]) {
            i++;
        }
        if (pfreq[n] != eq->freq[i]) {
            fprintf(stderr, "EQ: %.1f Hz is not a band frequency, "
                    "use %.1f instead.\n", pfreq[n], eq->freq[i]);
            return false;
        }
        eq->phase[i++] = phase[n];
    }
    for (n = 0; n < band_count; n++) {
        eq->freq[n] /= (double)sample_rate;
        eq->mag[n] = pow(10, eq->mag[n] / 20);
        eq->phase[n] = eq->phase[n] / (180 * M_PI);           
    }
    eq->band_count = band_count;
    for (n = i = 0; n < 2; n++) {
        if (!coeffs[eq->coeff[n]].is_shared) {
            fprintf(stderr, "EQ: Coefficient %d must be in shared memory.\n",
                    eq->coeff[n]);
            return false;
        }
        if ((i = log2_get(block_length * coeffs[eq->coeff[n]].n_blocks)) == -1)
        {
            fprintf(stderr, "EQ: Coefficient %d length is not a power "
                    "of two.\n", eq->coeff[n]);
            return false;
        }
    }
    eq->taps = 1 << i;
    if (coeffs[eq->coeff[0]].n_blocks != coeffs[eq->coeff[1]].n_blocks) {
        fprintf(stderr, "EQ: Coefficient %d and %d must be the same length.\n",
                eq->coeff[0], eq->coeff[1]);
        return false;
    }
    return true;
}

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "EQ: Parse error: " errstr);                           \
        return -1;                                                             \
    }

static int
parse_freq_val(int (*get_config_token)(union bflexval *lexval),
               double freq[],
               double val[])
{
    union bflexval lexval;
    int n, token;

    token = BF_LEX_COMMA;
    for (n = 0; n < MAX_BANDS && token == BF_LEX_COMMA; n++) {
        GET_TOKEN(BF_LEXVAL_REAL, "expected real.\n");
        freq[n] = lexval.real;
        if (freq[n] < 0) {
            fprintf(stderr, "EQ: Parse error: negative frequency.\n");
            return -1;
        }
        if (freq[n] > (double)sample_rate / 2.0) {
            fprintf(stderr, "EQ: Parse error: frequency larger than "
                    "nykvist.\n");
            return -1;
        }
        if (n > 0 && freq[n] <= freq[n-1]) {
            fprintf(stderr, "EQ: Parse error: frequencies not sorted.\n");
            return -1;
        }
        GET_TOKEN(BF_LEX_SLASH, "expected slash (/).\n");
        GET_TOKEN(BF_LEXVAL_REAL, "expected real.\n");
        val[n] = lexval.real;
        token = get_config_token(&lexval);
    }
    if (token != BF_LEX_EOS) {
        fprintf(stderr, "EQ: Parse error: expected end of statement (;).\n");
        return -1;
    }
    return n;
}

int
bflogic_preinit(int *version_major,
                int *version_minor,
                int (*get_config_token)(union bflexval *lexval),
                int _sample_rate,
                int _block_length,
                int _n_maxblocks,
                int _n_coeffs,
                const struct bfcoeff _coeffs[],
                const int _n_channels[2],
                const struct bfchannel *_channels[2],
                int _n_filters,
                const struct bffilter _filters[],
                struct bfevents *bfevents,
                int *fork_mode,
                int _debug)
{
    double mag[2][MAX_BANDS];
    double phase[2][MAX_BANDS];
    double bands[MAX_BANDS];
    int n_mag, n_phase, n_bands;
    union bflexval lexval;
    int token, n, i, ver;
    char *p;

    ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return -1;
    }
    debug = !!_debug;
    sample_rate = _sample_rate;
    block_length = _block_length;
    n_maxblocks = n_maxblocks;
    n_coeffs = _n_coeffs;
    coeffs = _coeffs;
    n_filters = _n_filters;
    *fork_mode = BF_FORK_PRIO_OTHER;
    bfevents->coeff_final = coeff_final;
    
    memset(msg, 0, sizeof(msg));
    if ((equalisers = shmalloc(MAX_EQUALISERS * sizeof(struct realtime_eq)))
        == NULL)
    {
        fprintf(stderr, "EQ: Could not allocate shared memory\n");
        return -1;
    }

    while ((token = get_config_token(&lexval)) > 0) {
        switch (token) {
        case BF_LEX_LBRACE:
            if (n_equalisers == MAX_EQUALISERS) {
                fprintf(stderr, "EQ: Too many equalisers.\n");
                return -1;
            }
            memset(&equalisers[n_equalisers], 0, sizeof(struct realtime_eq));
            equalisers[n_equalisers].coeff[0] = -1;
            equalisers[n_equalisers].coeff[1] = -1;
            n_mag = 0;
            n_phase = 0;
            n_bands = -1;
            while ((token = get_config_token(&lexval)) > 0) {
                if (token == BF_LEX_RBRACE) {
                    if (equalisers[n_equalisers].coeff[0] == -1) {
                        fprintf(stderr, "EQ: Parse error: coeff not set.\n");
                        return -1;
                    }
                    if (n_bands == -1) {
                        fprintf(stderr, "EQ: Parse error: bands not set.\n");
                        return -1;
                    }
                    if (!finalise_equaliser(&equalisers[n_equalisers],
                                            mag[0], mag[1], n_mag,
                                            phase[0], phase[1], n_phase,
                                            bands, n_bands))
                    {
                        return -1;
                    }
                    n_equalisers++;
                    break;
                }
                if (token != BF_LEXVAL_FIELD) {
                    fprintf(stderr, "EQ: Parse error: expected field.\n");
                    return -1;
                }
                if (strcmp(lexval.field, "bands") == 0) {
                    token = get_config_token(&lexval);
                    switch (token) {
                    case BF_LEXVAL_STRING:
                        if (strcmp("ISO octave", lexval.string) == 0) {
                            n_bands = 10;
                            bands[0] = 31.5;
                            bands[1] = 63;
                            bands[2] = 125;
                            bands[3] = 250;
                            bands[4] = 500;
                            bands[5] = 1000;
                            bands[6] = 2000;
                            bands[7] = 4000;
                            bands[8] = 8000;
                            bands[9] = 16000;
                        } else if (strcmp("ISO 1/3 octave", lexval.string)
                                   == 0)
                        {
                            n_bands = 31;
                            bands[0] = 20;
                            bands[1] = 25;
                            bands[2] = 31;
                            bands[3] = 40;
                            bands[4] = 50;
                            bands[5] = 63;
                            bands[6] = 80;
                            bands[7] = 100;
                            bands[8] = 125;
                            bands[9] = 160;
                            bands[10] = 200;
                            bands[11] = 250;
                            bands[12] = 315;
                            bands[13] = 400;
                            bands[14] = 500;
                            bands[15] = 630;
                            bands[16] = 800;
                            bands[17] = 1000;
                            bands[18] = 1250;
                            bands[19] = 1600;
                            bands[20] = 2000;
                            bands[21] = 2500;
                            bands[22] = 3150;
                            bands[23] = 4000;
                            bands[24] = 5000;
                            bands[25] = 6300;
                            bands[26] = 8000;
                            bands[27] = 10000;
                            bands[28] = 12500;
                            bands[29] = 16000;
                            bands[30] = 20000;
                        } else {
                            fprintf(stderr, "EQ: Parse error: expected \"ISO "
                                    "octave\" or \"ISO 1/3 octave\".\n");
                            return -1;
                        }
                        GET_TOKEN(BF_LEX_EOS, "expected end of statement "
                                  "(;).\n");
                        for (n = n_bands - 1; n > 0; n--) {
                            if (bands[n] < (double)sample_rate / 2) {
                                break;
                            }
                            n_bands--;
                        }
                        break;
                    case BF_LEXVAL_REAL:
                        bands[0] = lexval.real;
                        if (bands[0] <= 0.0) {
                            fprintf(stderr, "EQ: Parse error: band frequencies "
                                    "must be larger than 0 Hz.\n");
                            return -1;
                        }
                        token = get_config_token(&lexval);
                        for (n = 1;
                             n < MAX_BANDS && token == BF_LEX_COMMA;
                             n++)
                        {
                            GET_TOKEN(BF_LEXVAL_REAL, "expected real.\n");
                            bands[n] = lexval.real;
                            if (bands[n-1] >= bands[n]) {
                                fprintf(stderr, "EQ: Parse error: frequencies "
                                        "not sorted.\n");
                                return -1;                            
                            }
                            token = get_config_token(&lexval);
                        }
                        n_bands = n;
                        if (token != BF_LEX_EOS) {
                            fprintf(stderr, "EQ: Parse error: expected end of "
                                    "statement (;).\n");
                            return -1;
                        }
                        break;
                    default:
                        fprintf(stderr, "EQ: Parse error: expected real.\n");
                        return -1;
                    }
                    if (bands[n_bands-1] >= (double)sample_rate / 2.0) {
                        fprintf(stderr, "EQ: Parse error: band frequencies "
                                "must be less than sample rate / 2.\n");
                        return -1;
                    }
                } else if (strcmp(lexval.field, "coeff") == 0) {
                    for (i = 0; i < 2; i++) {
                        token = get_config_token(&lexval);
                        if (token != BF_LEXVAL_STRING &&
                            token != BF_LEXVAL_REAL)
                        {
                            fprintf(stderr, "EQ: Parse error: expected integer "
                                    "or string.\n");
                            return -1;                            
                        }
                        if (token == BF_LEXVAL_STRING) {
                            for (n = 0; n < n_coeffs; n++) {
                                if (strcmp(coeffs[n].name,
                                           lexval.string) == 0)
                                {
                                    equalisers[n_equalisers].coeff[i] = n;
                                    break;
                                }
                            }
                            if (n == n_coeffs) {
                                fprintf(stderr, "EQ: Unknown coefficient "
                                        "name.\n");
                                return -1;
                            }
                        } else {
                            equalisers[n_equalisers].coeff[i] =
                                (int)lexval.real;
                            if (equalisers[n_equalisers].coeff[i] < 0 ||
                                equalisers[n_equalisers].coeff[i] >= n_coeffs)
                            {
                                fprintf(stderr, "EQ: Invalid coefficient "
                                        "index.\n");
                                return -1;
                            }
                        }
                        token = get_config_token(&lexval);
                        if (i == 0) {
                            if (token == BF_LEX_EOS) {
                                equalisers[n_equalisers].coeff[1] =
                                    equalisers[n_equalisers].coeff[0];
                                break;
                            } else if (token != BF_LEX_COMMA) {
                                fprintf(stderr, "EQ: Parse error: expected "
                                        "comma.\n");
                                return -1;
                            }
                        } else if (token != BF_LEX_EOS) {
                            fprintf(stderr, "EQ: Parse error: expected end of "
                                    "statement (;).\n");
                            return -1;
                        }
                    }
                } else if (strcmp(lexval.field, "magnitude") == 0) {
                    if ((n_mag = parse_freq_val(get_config_token,
                                                mag[0], mag[1])) == -1)
                    {
                        return -1;
                    }
                } else if (strcmp(lexval.field, "phase") == 0) {
                    if ((n_phase = parse_freq_val(get_config_token,
                                                  phase[0], phase[1])) == -1)
                    {
                        return -1;
                    }
                } else {
                    fprintf(stderr, "EQ: Parse error: unknown field \"%s\".\n",
                            lexval.field);
                    return -1;                
                }
            }
            break;
        case BF_LEXVAL_FIELD:
            if (strcmp(lexval.field, "debug_dump_filter") == 0) {
                GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
                debug_dump_filter_path = estrdup(lexval.string);
                if (strstr(debug_dump_filter_path, "%d") == NULL) {
                    fprintf(stderr, "EQ: Parse error: %%d is missing in "
                            "name.\n");
                    return -1;
                }
                p = strchr(debug_dump_filter_path, '%');
                if (strchr(&p[1], '%')) {
                    fprintf(stderr, "EQ: Parse error: more than one %% in "
                            "name.\n");
                    return -1;
                }
            } else {
                fprintf(stderr, "EQ: Parse error: unknown field.\n");
                return -1;
            }
            break;
        default:
            fprintf(stderr, "EQ: Parse error: expected field.\n");
            return -1;
        }
        GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
    }
    
    for (n = 0; n < n_equalisers; n++) {
        for (i = 0; i < n_equalisers; i++) {
            if (i != n &&
                (equalisers[n].coeff[0] == equalisers[i].coeff[0] ||
                 equalisers[n].coeff[0] == equalisers[i].coeff[1] ||
                 equalisers[n].coeff[1] == equalisers[i].coeff[0] ||
                 equalisers[n].coeff[1] == equalisers[i].coeff[1]))
            {
                fprintf(stderr, "EQ: At least two equalisers has at least one "
                        "coefficient set in common.\n");
                return -1;
            }
        }
    }

    if (pipe(cmdpipe) == -1 || pipe(cmdpipe_reply) == -1) {
        fprintf(stderr, "EQ: Failed to create pipe: %s.\n", strerror(errno));
        return -1;
    }
    
    return 0;
}

int
bflogic_init(struct bfaccess *_bfaccess,
	     int _sample_rate,
	     int _block_length,
	     int _n_maxblocks,
	     int _n_coeffs,
	     const struct bfcoeff _coeffs[],
	     const int _n_channels[2],
	     const struct bfchannel *_channels[2],
	     int _n_filters,
	     const struct bffilter _filters[],
	     int event_fd,
             int synch_fd)
{
    int n, maxblocks, command, eq_index, n_bands, i;
    double bands[MAX_BANDS], values[MAX_BANDS], freq;
    int render_postponed_index = -1;
    struct realtime_eq *eq;
    char rmsg[MSGSIZE], *p;
    uint8_t dummy = 0;
    struct timeval tv;
    fd_set rfds;

    bfaccess = _bfaccess;

    FD_ZERO(&rfds);
    maxblocks = 0;
    for (n = 0; n < n_equalisers; n++) {
        if (maxblocks < coeffs[equalisers[n].coeff[0]].n_blocks) {
            maxblocks = coeffs[equalisers[n].coeff[0]].n_blocks;
        }
    }
    rbuf = emallocaligned(maxblocks * block_length * bfaccess->realsize);
    
    for (n = 0; n < n_equalisers; n++) {
        equalisers[n].ifftplan =
            bfaccess->convolver_fftplan(log2_get(equalisers[n].taps), true,
                                        true);
        if (bfaccess->realsize == 4) {
            render_equaliser_f(&equalisers[n]);
        } else {
            render_equaliser_d(&equalisers[n]);
        }
    }
    if (!writefd(synch_fd, &dummy, 1)) {
        fprintf(stderr, "EQ: write failed.\n");
        return -1;
    }
    close(synch_fd);
    while (true) {
        if (!readfd(cmdpipe[0], &command, sizeof(int)) ||
            !readfd(cmdpipe[0], &eq_index, sizeof(int)))
        {
            fprintf(stderr, "EQ: read failed.\n");
            return -1;
        }
        eq = &equalisers[eq_index];
        switch (command) {
        case CMD_CHANGE_MAGNITUDE:
        case CMD_CHANGE_PHASE:
            if (!readfd(cmdpipe[0], &n_bands, sizeof(int)) ||
                !readfd(cmdpipe[0], bands, n_bands * sizeof(double)) ||
                !readfd(cmdpipe[0], values, n_bands * sizeof(double)))
            {
                fprintf(stderr, "EQ: read failed.\n");
                return -1;
            }
            for (n = 0, i = 0; i < n_bands && n < eq->band_count; n++) {
                if (bands[i] > 0.99 * eq->freq[n] &&
                    bands[i] < 1.01 * eq->freq[n])
                {
                    if (command == CMD_CHANGE_MAGNITUDE) {
                        eq->mag[n] = values[i];
                    } else {
                        eq->phase[n] = values[i];
                    }
                    i++;
                }
            }
            if (render_postponed_index == eq_index) {
                render_postponed_index = -1;
            }
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            FD_SET(cmdpipe[0], &rfds);
            if (select(cmdpipe[0] + 1, &rfds, NULL, NULL, &tv) == 1) {
                /* we have a command waiting, so we postpone render
                   equaliser a while */
                if (render_postponed_index != -1) {
                    if (bfaccess->realsize == 4) {
                        render_equaliser_f(&equalisers[render_postponed_index]);
                    } else {
                        render_equaliser_d(&equalisers[render_postponed_index]);
                    }
                }
                render_postponed_index = eq_index;
                continue;
            }
            if (bfaccess->realsize == 4) {
                render_equaliser_f(eq);
            } else {
                render_equaliser_d(eq);
            }
            break;
        case CMD_GET_INFO:
            p = rmsg;
            memset(rmsg, 0, sizeof(rmsg));
            if (eq->coeff[0] == eq->coeff[1]) {
                sprintf(p, "coefficient %d:\n band: ", eq->coeff[0]);
            } else {
                sprintf(p, "coefficient %d,%d:\n band: ",
                        eq->coeff[0], eq->coeff[1]);
            }
            p += strlen(p);
            for (n = 1; n < eq->band_count - 1; n++) {
                freq = eq->freq[n] * (double)sample_rate;
                if (freq < 100) {
                    sprintf(p, "%6.1f", freq);
                } else {
                    sprintf(p, "%6.0f", freq);
                }
                p += strlen(p);
            }
            sprintf(p, "\n  mag: ");
            p += strlen(p);
            for (n = 1; n < eq->band_count - 1; n++) {
                sprintf(p, "%6.1f", 20 * log10(eq->mag[n]));
                p += strlen(p);
            }
            sprintf(p, "\nphase: ");
            p += strlen(p);
            for (n = 1; n < eq->band_count - 1; n++) {
                sprintf(p, "%6.1f", M_PI * 180 * eq->phase[n]);
                p += strlen(p);
            }
            sprintf(p, "\n");
            if (!writefd(cmdpipe_reply[1], rmsg, sizeof(rmsg))) {
                fprintf(stderr, "EQ: write failed.\n");
                return -1;
            }
            break;
        }
        if (render_postponed_index != -1) {
            if (bfaccess->realsize == 4) {
                render_equaliser_f(&equalisers[render_postponed_index]);
            } else {
                render_equaliser_d(&equalisers[render_postponed_index]);
            }
            render_postponed_index = -1;
        }
    }
    return 0;
}

int
bflogic_command(const char params[])
{
    int command, coeff, n, i, n_bands, eq_index;
    char *p, *params_copy, *cmd;
    double bands[MAX_BANDS], values[MAX_BANDS];
    struct realtime_eq *eq;

    params_copy = estrdup(params);
    cmd = strtrim(params_copy);
    coeff = -1;
    
    /* <coeff> <mag | phase | info> <band>/<value>[,<band/value>, ...] */
    if (cmd[0] == '\"') {
        p = strchr(cmd + 1, '\"');
        if (p == NULL) {
            sprintf(msg, "Invalid coefficient.\n");
            free(params_copy);
            return -1;
        }
        *p = '\0';
        p++;
        for (n = 0; n < n_coeffs; n++) {
            if (strcmp(cmd + 1, coeffs[n].name) == 0) {
                coeff = coeffs[n].intname;
                break;
            }
        }
        if (n == n_coeffs) {
            sprintf(msg, "Coefficient with name \"%s\" does not exist.\n",
                    cmd + 1);
            free(params_copy);
            return -1;
        }
    } else {
        coeff = strtol(cmd, &p, 10);
        if (p == cmd) {
            sprintf(msg, "Invalid number.\n");
            free(params_copy);
            return -1;
        }
    }
    for (n = 0; n < n_equalisers; n++) {
        if (equalisers[n].coeff[0] == coeff ||
            equalisers[n].coeff[1] == coeff)
        {
            eq_index = n;
            break;
        }
    }
    if (n == n_equalisers) {
        sprintf(msg, "The given coefficient is not controlled.\n");
        free(params_copy);
        return -1;
    }
    cmd = strtrim(p);
    if (strstr(cmd, "mag") == cmd) {
        command = CMD_CHANGE_MAGNITUDE;
        cmd = strtrim(cmd + 3);
    } else if (strstr(cmd, "phase") == cmd) {
        command = CMD_CHANGE_PHASE;
        cmd = strtrim(cmd + 5);
    } else if (strstr(cmd, "info") == cmd) {
        command = CMD_GET_INFO;
    } else {
        sprintf(msg, "Unknown command.\n");
        free(params_copy);
        return -1;
    }

    switch (command) {
    case CMD_CHANGE_MAGNITUDE:
    case CMD_CHANGE_PHASE:
        for (n = 0; n < MAX_BANDS && cmd[0] != '\0'; n++) {
            bands[n] = strtod(cmd, &p);
            if (p == cmd || *p != '/') {
                sprintf(msg, "Invalid frequency/value list.\n");
                free(params_copy);
                return -1;
            }
            if (n > 1 && bands[n] <= bands[n-1]) {
                sprintf(msg, "Frequency bands not sorted.\n");
                free(params_copy);
                return -1;
            }
            cmd = p + 1;
            values[n] = strtod(cmd, &p);
            if (p == cmd) {
                sprintf(msg, "Invalid frequency/value list.\n");
                free(params_copy);
                return -1;
            }
            cmd = strtrim(p);
            if (cmd[0] != ',' && cmd[0] != '\0') {
                sprintf(msg, "Invalid frequency/value list.\n");
                free(params_copy);
                return -1;
            }
            if (cmd[0] == ',') {
                cmd++;
            }
        }
        free(params_copy);
        n_bands = n;
        eq = &equalisers[eq_index];
        for (n = i = 0; i < n_bands && n < eq->band_count; n++) {
            if (bands[i] / (double)sample_rate > 0.99 * eq->freq[n] &&
                bands[i] / (double)sample_rate < 1.01 * eq->freq[n])
            {
                bands[i] /= (double)sample_rate;
                if (command == CMD_CHANGE_MAGNITUDE) {                    
                    values[i] = pow(10, values[i] / 20);
                } else {
                    values[i] = values[i] / (180 * M_PI);
                }
                i++;
            }
        }
        if (i != n_bands) {
            sprintf(msg, "At least one invalid frequency band.\n");
            return -1;
        }
        /* <int: command><int: eq index><int: n_bands>
           <double: bands><double: values> */
        if (!writefd(cmdpipe[1], &command, sizeof(int)) ||
            !writefd(cmdpipe[1], &eq_index, sizeof(int)) ||
            !writefd(cmdpipe[1], &n_bands, sizeof(int)) ||
            !writefd(cmdpipe[1], bands, n_bands * sizeof(double)) ||
            !writefd(cmdpipe[1], values, n_bands * sizeof(double)))
        {
            sprintf(msg, "Write failed: %s.\n", strerror(errno));
            return -1;
        }
        sprintf(msg, "ok\n");
        break;
    case CMD_GET_INFO:
        if (!writefd(cmdpipe[1], &command, sizeof(int)) ||
            !writefd(cmdpipe[1], &eq_index, sizeof(int)))
        {
            sprintf(msg, "Write failed: %s.\n", strerror(errno));
            return -1;
        }
        if (!readfd(cmdpipe_reply[0], msg, sizeof(msg))) {
            sprintf(msg, "Write failed: %s.\n", strerror(errno));
            return -1;
        }
        break;
    }       
    return 0;
}

const char *
bflogic_message(void)
{
    return msg;
}
