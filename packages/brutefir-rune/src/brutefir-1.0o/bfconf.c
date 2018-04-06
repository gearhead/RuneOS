/*
 * (c) Copyright 2001 - 2006, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include "defs.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <sys/types.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pwd.h>
#include <sys/shm.h>
#include <sched.h>
#include <dlfcn.h>
#include <stddef.h>

#include "bfrun.h"
#include "bfconf.h"
#include "emalloc.h"
#include "log2.h"
#include "bit.h"
#include "dai.h"
#include "bfconf_grammar.h"
#include "timermacros.h"
#include "shmalloc.h"
#include "dither.h"
#include "inout.h"
#include "swap.h"
#include "bfmod.h"
#include "pinfo.h"
#include "numunion.h"
#include "delay.h"

#define PATH_SEPARATOR_CHAR '/'
#define PATH_SEPARATOR_STR "/"
#define CONVOLVER_NEEDS_CONFIGFILE 1

#define MINFILTERLEN 4
#define MAXFILTERLEN (1 << 30)

struct bflex {
    int line;
    int token;
    union bflexval lexval;
};

struct coeff {
    struct bfcoeff coeff;
#define COEFF_FORMAT_RAW 1    
#define COEFF_FORMAT_TEXT 3
#define COEFF_FORMAT_PROCESSED 4
    int format;
    int skip;
    struct sample_format rawformat;
    char filename[PATH_MAX];
    int shm_shmids[BF_MAXCOEFFPARTS];
    int shm_offsets[BF_MAXCOEFFPARTS];
    int shm_blocks[BF_MAXCOEFFPARTS];
    int shm_elements;
    double scale;
};

struct filter {
    struct bffilter filter;
    struct bffilter_control fctrl;
    char coeff_name[BF_MAXOBJECTNAME];
    char *channel_name[2][BF_MAXCHANNELS];
    char *filter_name[2][BF_MAXCHANNELS];
    int process;
};

struct iodev {
    int virtual_channels;
    int channel_intname[BF_MAXCHANNELS];
    char *channel_name[BF_MAXCHANNELS];
    int virt2phys[BF_MAXCHANNELS];
    struct dai_channels ch; /* physical channels */
    struct bflex *device_params;
    char device_name[BF_MAXOBJECTNAME];
    int maxdelay;
    bool_t apply_dither;
    bool_t auto_format;
};

union bflexval yylval;
struct bfconf *bfconf = NULL;
uint64_t base_ts = 0;

static struct coeff *default_coeff = NULL;
static struct filter *default_filter = NULL;
static struct iodev *default_iodev[2] = { NULL, NULL };
static char *convolver_config = NULL;
static char default_config_file[PATH_MAX];
static char current_filename[PATH_MAX];
static char *modules_path = NULL;
static char *logic_names[BF_MAXMODULES];
static struct bflex *logic_params[BF_MAXMODULES];
static struct bflex *config_params;
static int config_params_pos;
static bool_t has_defaults = false;

#define FROM_DB(db) (pow(10, (db) / 20.0))

void
parse_error(const char msg[])
{
    fprintf(stderr, "Parse error on line %d in file \"%s\":\n  %s",
	    lexlineno, current_filename, msg);
    exit(BF_EXIT_INVALID_CONFIG);
}

static char *
tilde_expansion(const char path[])
{
    static char real_path[PATH_MAX];
    struct passwd *pw;
    char *homedir = NULL, *p;
    int pos, slashpos = -1;
    
    /* FIXME: cleanup the code */
    
    if (path[0] != '~') {
	strncpy(real_path, path, PATH_MAX);
	real_path[PATH_MAX-1] = '\0';
	return real_path;
    }

    if ((p = strchr(path, PATH_SEPARATOR_CHAR)) != NULL) {
	slashpos = p - path;
    }
    
    if (path[1] == '\0' || slashpos == 1) {
	if ((homedir = getenv("HOME")) == NULL)
	{
	    if ((pw = getpwuid(getuid())) != NULL) {
		homedir = pw->pw_dir;
	    }
	}
    } else {
	strncpy(real_path, &path[1], PATH_MAX);
	real_path[PATH_MAX-1] = '\0';
	if (slashpos != -1) {
	    real_path[slashpos-1] = '\0';
	}
	if ((pw = getpwnam(real_path)) != NULL) {
	    homedir = pw->pw_dir;
	}
    }
    if (homedir == NULL) {
	strncpy(real_path, path, PATH_MAX);
	real_path[PATH_MAX-1] = '\0';
	return real_path;
    }
    
    real_path[PATH_MAX-1] = '\0';
    strncpy(real_path, homedir, PATH_MAX - 2);
    pos = strlen(homedir);
    if (homedir[0] == '\0' ||
	homedir[strlen(homedir)-1] != PATH_SEPARATOR_CHAR)
    {
	strcat(real_path, PATH_SEPARATOR_STR);
	pos += 1;
    }
    if (slashpos != -1) {
	strncpy(&real_path[pos], &path[slashpos+1], PATH_MAX - pos - 1);
    } else {
	strncpy(&real_path[pos], &path[1], PATH_MAX - pos - 1);
    }
    real_path[PATH_MAX-1] = '\0';
    
    return real_path;
}

static void
create_default_config(void)
{
    FILE *stream;

    if ((stream = fopen(tilde_expansion(DEFAULT_BFCONF_NAME), "wt")) == NULL) {
	fprintf(stderr, "Could not create default configuration file ("
		DEFAULT_BFCONF_NAME "): %s.\n", strerror(errno));
	exit(BF_EXIT_OTHER);
    }
    fprintf(stream, "## DEFAULT GENERAL SETTINGS ##\n\
\n\
float_bits: 32;             # internal floating point precision\n\
sampling_rate: 44100;       # sampling rate in Hz of audio interfaces\n\
filter_length: 65536;       # length of filters\n\
config_file: \"~/.brutefir_config\"; # standard location of main config file\n\
overflow_warnings: true;    # echo warnings to stderr if overflow occurs\n\
show_progress: true;        # echo filtering progress to stderr\n\
max_dither_table_size: 0;   # maximum size in bytes of precalculated dither\n\
allow_poll_mode: false;     # allow use of input poll mode\n\
modules_path: \".\";          # extra path where to find BruteFIR modules\n\
monitor_rate: false;        # monitor sample rate\n\
powersave: false;           # pause filtering when input is zero\n\
lock_memory: true;          # try to lock memory if realtime prio is set\n\
sdf_length: -1;             # subsample filter half length in samples\n\
safety_limit: 20;           # if non-zero max dB in output before aborting\n"
#ifdef CONVOLVER_NEEDS_CONFIGFILE
	    "convolver_config: \"~/.brutefir_convolver\"; # location of "
	    "convolver config file\n"
#endif
"\n\
## COEFF DEFAULTS ##\n\
\n\
coeff {\n\
\tformat: \"TEXT\";     # file format\n\
\tattenuation: 0.0;   # attenuation in dB\n\
\tblocks: -1;         # how long in blocks\n\
\tskip: 0;            # how many bytes to skip\n\
\tshared_mem: false;  # allocate in shared memory\n\
};\n\
\n\
## INPUT DEFAULTS ##\n\
\n\
input {\n\
\tdevice: \"file\" {};  # module and parameters to get audio\n\
\tsample: \"S16_LE\";   # sample format\n\
\tchannels: 2/0,1;    # number of open channels / which to use\n\
\tdelay: 0,0;         # delay in samples for each channel\n\
\tmaxdelay: -1;       # max delay for variable delays\n\
\tsubdelay: 0,0;      # subsample delay in 1/%dth sample for each channel\n\
\tmute: false,false;  # mute active on startup for each channel\n\
};\n\
\n\
## OUTPUT DEFAULTS ##\n\
\n\
output {\n\
\tdevice: \"file\" {};  # module and parameters to put audio\n\
\tsample: \"S16_LE\";   # sample format\n\
\tchannels: 2/0,1;    # number of open channels / which to use\n\
\tdelay: 0,0;         # delay in samples for each channel\n\
\tmaxdelay: -1;       # max delay for variable delays\n\
\tsubdelay: 0,0;      # subsample delay in 1/%dth sample for each channel\n\
\tmute: false,false;  # mute active on startup for each channel\n\
\tdither: false;      # apply dither\n\
};\n\
\n\
## FILTER DEFAULTS ##\n\
\n\
filter {\n\
\tprocess: -1;        # process index to run in (-1 means auto)\n\
\tdelay: 0;           # predelay, in blocks\n\
\tcrossfade: false;   # crossfade when coefficient is changed\n\
};\n\
", BF_SAMPLE_SLOTS, BF_SAMPLE_SLOTS);
	    
    fclose(stream);
}

static const char *
token_name(int token)
{
    switch (token) {
    case REAL:
	return "number";
    case BOOLEAN:
	return "boolean";
    case STRING:
	return "string";
    case FIELD:
	return "field";
    case EOS:
	return "end of statement (;)";
    case LBRACE:
	return "left brace ({)";
    case RBRACE:
	return "right brace (})";
    case COMMA:
	return "comma (,)";
    case SLASH:
	return "slash (/)";
    case EOF:
	return "end of file";
    case COEFF:
	return "coeff";
    case INPUT:
	return "input";
    case OUTPUT:
	return "output";
    case FILTER:
	return "filter";
    default:
	return "UNKNOWN";
    }
}

static void
unexpected_token(int expected_token,
		 int yylex_ret)
{
    char msg[4096];
    sprintf(msg, "unexpected token, expected %s, got %s.\n",
	    token_name(expected_token), token_name(yylex_ret));
    parse_error(msg);
}

static void
unrecognised_token(const char name[],
		   const char string[])
{
    char msg[4096];
    sprintf(msg, "unrecognised %s: \"%s\".\n", name, string);
    parse_error(msg);
}

static int
make_integer(double number)
{
    if (rint(number) != number) {
	parse_error("Expected integer, got floating point.\n");
    }
    return (int)number;
}

static void
get_token(int token)
{
    int _token;
    if ((_token = yylex()) != token) {
	unexpected_token(token, _token);
    }
}

static void
field_repeat_test(uint32_t *bitset,
		  int bit)
{
    if (bit_isset(bitset, bit)) {
	parse_error("Field is already set.\n");
    }    
    bit_set(bitset, bit);
}

static void
field_mandatory_test(uint32_t bitset,
		     uint32_t bits,
		     const char name[])
{
    char msg[200];
    
    if ((bitset & bits) != bits) {
	sprintf(msg, "At least one mandatory field is missing in %s.\n", name);
	parse_error(msg);
    }
}

static bool_t
parse_sample_format(struct sample_format *sf,
		    const char s[],
                    bool_t allow_auto)
{
    bool_t little_endian, native_endian;

    little_endian = true;
    native_endian = false;
    memset(sf, 0, sizeof(struct sample_format));

    if (strcasecmp(s, "AUTO") == 0) {
        if (allow_auto) {
            return true;
        }
	parse_error("Cannot have \"AUTO\" sample format here.\n");
    }
    
    /* new sample format string format */
    if (strcasecmp(s, "S8") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S8;
	sf->bytes = 1;
	sf->sbytes = 1;
	little_endian = false;
    } else if (strcasecmp(s, "S16_LE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S16_LE;
	sf->bytes = 2;
	sf->sbytes = 2;
    } else if (strcasecmp(s, "S16_BE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S16_BE;
	sf->bytes = 2;
	sf->sbytes = 2;
	little_endian = false;
    } else if (strcasecmp(s, "S16_NE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S16_NE;
	sf->bytes = 2;
	sf->sbytes = 2;
        native_endian = true;
    } else if (strcasecmp(s, "S24_LE") == 0 || strcasecmp(s, "S24_3LE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S24_LE;
	sf->bytes = 3;
	sf->sbytes = 3;
    } else if (strcasecmp(s, "S24_BE") == 0 || strcasecmp(s, "S24_3BE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S24_BE;
	sf->bytes = 3;
	sf->sbytes = 3;
	little_endian = false;
    } else if (strcasecmp(s, "S24_NE") == 0 || strcasecmp(s, "S24_3NE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S24_NE;
	sf->bytes = 3;
	sf->sbytes = 3;
        native_endian = true;
    } else if (strcasecmp(s, "S24_4LE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S24_4LE;
	sf->bytes = 4;
	sf->sbytes = 3;
    } else if (strcasecmp(s, "S24_4BE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S24_4BE;
	sf->bytes = 4;
	sf->sbytes = 3;
	little_endian = false;
    } else if (strcasecmp(s, "S24_4NE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S24_4NE;
	sf->bytes = 4;
	sf->sbytes = 3;
        native_endian = true;
    } else if (strcasecmp(s, "S32_LE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S32_LE;
	sf->bytes = 4;
	sf->sbytes = 4;
    } else if (strcasecmp(s, "S32_BE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S32_BE;
	sf->bytes = 4;
	sf->sbytes = 4;
	little_endian = false;
    } else if (strcasecmp(s, "S32_NE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_S32_NE;
	sf->bytes = 4;
	sf->sbytes = 4;
        native_endian = true;
    } else if (strcasecmp(s, "FLOAT_LE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_FLOAT_LE;
	sf->bytes = 4;
	sf->sbytes = 4;
	sf->isfloat = true;
    } else if (strcasecmp(s, "FLOAT_BE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_FLOAT_BE;
	sf->bytes = 4;
	sf->sbytes = 4;
	sf->isfloat = true;
	little_endian = false;
    } else if (strcasecmp(s, "FLOAT_NE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_FLOAT_NE;
	sf->bytes = 4;
	sf->sbytes = 4;
        native_endian = true;
    } else if (strcasecmp(s, "FLOAT64_LE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_FLOAT64_LE;
	sf->bytes = 8;
	sf->sbytes = 8;
	sf->isfloat = true;
    } else if (strcasecmp(s, "FLOAT64_BE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_FLOAT64_BE;
	sf->bytes = 8;
	sf->sbytes = 8;
	sf->isfloat = true;
	little_endian = false;
    } else if (strcasecmp(s, "FLOAT64_NE") == 0) {
	sf->format = BF_SAMPLE_FORMAT_FLOAT64_NE;
	sf->bytes = 4;
	sf->sbytes = 4;
        native_endian = true;
    } else {
	parse_error("Unknown sample format.\n");
    }
    if (sf->isfloat) {
	sf->scale = 1.0;
    } else {
	sf->scale = 1.0 / (double)((uint64_t)1 << ((sf->sbytes << 3) - 1));
    }
#ifdef __BIG_ENDIAN__
    if (native_endian) {
        sf->swap = false;
        switch (sf->format) {
        case BF_SAMPLE_FORMAT_S16_NE:
            sf->format = BF_SAMPLE_FORMAT_S16_BE;
            break;
        case BF_SAMPLE_FORMAT_S24_NE:
            sf->format = BF_SAMPLE_FORMAT_S24_BE;
            break;
        case BF_SAMPLE_FORMAT_S24_4NE:
            sf->format = BF_SAMPLE_FORMAT_S24_4LE;
            break;
        case BF_SAMPLE_FORMAT_S32_NE:
            sf->format = BF_SAMPLE_FORMAT_S32_BE;
            break;
        case BF_SAMPLE_FORMAT_FLOAT_NE:
            sf->format = BF_SAMPLE_FORMAT_FLOAT_BE;
            break;
        case BF_SAMPLE_FORMAT_FLOAT64_NE:
            sf->format = BF_SAMPLE_FORMAT_FLOAT64_BE;
            break;
        }
    } else {
        sf->swap = little_endian;
    }
#endif        
#ifdef __LITTLE_ENDIAN__
    if (native_endian) {
        switch (sf->format) {
        case BF_SAMPLE_FORMAT_S16_NE:
            sf->format = BF_SAMPLE_FORMAT_S16_LE;
            break;
        case BF_SAMPLE_FORMAT_S24_NE:
            sf->format = BF_SAMPLE_FORMAT_S24_LE;
            break;
        case BF_SAMPLE_FORMAT_S24_4NE:
            sf->format = BF_SAMPLE_FORMAT_S24_4LE;
            break;
        case BF_SAMPLE_FORMAT_S32_NE:
            sf->format = BF_SAMPLE_FORMAT_S32_LE;
            break;
        case BF_SAMPLE_FORMAT_FLOAT_NE:
            sf->format = BF_SAMPLE_FORMAT_FLOAT_LE;
            break;
        case BF_SAMPLE_FORMAT_FLOAT64_NE:
            sf->format = BF_SAMPLE_FORMAT_FLOAT64_LE;
            break;
        }
    } else {
        sf->swap = !little_endian;
    }
#endif

    return false;
}

static void
free_params(struct bflex *bflex)
{
    int n;
    
    if (bflex == NULL) {
        return;
    }
    for (n = 0; bflex[n].token != 0; n++) {
        switch (bflex[n].token) {
        case BF_LEXVAL_STRING:
            efree(bflex[n].lexval.string);
            break;
        case BF_LEXVAL_FIELD:
            efree(bflex[n].lexval.field);
            break;
        }
    }
    efree(bflex);
}

static struct bflex *
get_params(void)
{
    int brace_depth, n, capacity;
    struct bflex *bflex;
    
    get_token(LBRACE);
    bflex = NULL;
    brace_depth = 0;
    capacity = 0;
    n = 0;
    while (true) {
        if (n == capacity) {
            capacity += 10;
            bflex = erealloc(bflex, capacity * sizeof(struct bflex));
        }
        bflex[n].token = yylex();
        bflex[n].line = lexlineno;
        memcpy(&bflex[n].lexval, &yylval, sizeof(yylval));
        switch (bflex[n].token) {
        case BF_LEX_LBRACE:
            brace_depth++;
            break;
        case BF_LEX_RBRACE:
            if (brace_depth == 0) {
                /* finished */
                bflex[n].token = 0;
                return bflex;
            }
            brace_depth--;
            break;
        case BF_LEXVAL_STRING:
            bflex[n].lexval.string = estrdup(yylval.string);
            break;
        case BF_LEXVAL_FIELD:
            bflex[n].lexval.field = estrdup(yylval.field);
            break;
        }
        n++;
    }    
}

static int
get_config_token(union bflexval *lexval)
{
    if (config_params[config_params_pos].token == 0) {
        return 0;
    }
    lexlineno = config_params[config_params_pos].line;
    memcpy(lexval, &config_params[config_params_pos].lexval,
           sizeof(union bflexval));
    return config_params[config_params_pos++].token;
}

/* FIXME: get_string_list and get_integer_list is very similar */
static int
get_string_list(char firststring[],
		char *list[],
		int maxelements,
		int ending_token)
{
    char *str = firststring;
    int pos = 0, token;
    bool_t expecting_string = false;

    do {
	if (!expecting_string) {
	    if (pos == maxelements) {
		parse_error("String array is too long.\n");
	    }
	    list[pos] = estrdup(str);
	    pos++;
	}
	switch (token = yylex()) {
	case STRING:
	    if (!expecting_string) {
		unexpected_token(ending_token, token);
	    }
	    str = yylval.string;
	    expecting_string = false;
	    break;
	default:
	    if (token != ending_token) {
		unexpected_token(expecting_string ? STRING : ending_token,
				 token);
	    }
	    /* fall through */
	case COMMA:
	    if (expecting_string) {
		unexpected_token(STRING, token);
	    }
	    expecting_string = true;
	    break;
	}
    } while (token != ending_token);

    return pos;    
}

static int
get_integer_list(double firstnum,
		 int list[],
		 int maxelements,
		 int ending_token)
{
    double num = firstnum;
    int pos = 0, token;
    bool_t expecting_real = false;

    do {
	if (!expecting_real) {
	    if (pos == maxelements) {
		parse_error("Integer array is too long.\n");
	    }
	    list[pos] = make_integer(num);
	    pos++;
	}
	switch (token = yylex()) {
	case REAL:
	    if (!expecting_real) {
		unexpected_token(ending_token, token);
	    }
	    num = yylval.real;
	    expecting_real = false;
	    break;
	default:
	    if (token != ending_token) {
		unexpected_token(expecting_real ? REAL : ending_token,
				 token);
	    }
	    /* fall through */
	case COMMA:
	    if (expecting_real) {
		unexpected_token(REAL, token);
	    }
	    expecting_real = true;
	    break;
	}
    } while (token != ending_token);

    return pos;
}

/* returns true if integer, returns false if string */
static bool_t
get_string_or_int(char str[],
		  int strmax,
		  int *integer)
{
    int token;

    switch (token = yylex()) {
    case REAL:
	*integer = make_integer(yylval.real);
	str[0] = '\0';
	return true;
    case STRING:
	strncpy(str, yylval.string, strmax);
	str[strmax-1] = '\0';
	*integer = 0;
	return false;
    default:
	unexpected_token(STRING, token);
    }
    /* never reached */
    return false;
}

static struct coeff *
parse_coeff(bool_t parse_default,
	    int intname)
{
    struct coeff *coeff;
    uint32_t bitset = 0;
    int token, n;

    coeff = emalloc(sizeof(struct coeff));
    if (!parse_default) {
        if (has_defaults) {
            memcpy(coeff, default_coeff, sizeof(struct coeff));
        } else {
            memset(coeff, 0, sizeof(struct coeff));
            coeff->scale = 1.0;
            coeff->coeff.n_blocks = -1;
        }
        if (get_string_or_int(coeff->coeff.name, BF_MAXOBJECTNAME,
			      &coeff->coeff.intname))
	{
	    if (coeff->coeff.intname != intname) {
		parse_error("Incorrect integer name.\n");
	    }
	    sprintf(coeff->coeff.name, "%d", intname);
	}
	coeff->coeff.intname = intname;
    } else {
	memset(coeff, 0, sizeof(struct coeff));
	coeff->scale = 1.0;
    }

    get_token(LBRACE);

    do {
	switch (token = yylex()) {
	case FIELD:
	    if (strcmp(yylval.field, "format") == 0) {
		field_repeat_test(&bitset, 0);
		get_token(STRING);
		coeff->rawformat.isfloat = true;
		coeff->rawformat.swap = false;
		coeff->rawformat.bytes = sizeof(float);
		coeff->rawformat.sbytes = sizeof(float);
		coeff->rawformat.scale = 1.0;
		if (strcasecmp(yylval.string, "text") == 0) {
		    coeff->format = COEFF_FORMAT_TEXT;
		} else if (strcasecmp(yylval.string, "processed") == 0) {
		    coeff->format = COEFF_FORMAT_PROCESSED;
		} else {
		    coeff->format = COEFF_FORMAT_RAW;
		    parse_sample_format(&coeff->rawformat, yylval.string,
                                        false);
		}
		get_token(EOS);
	    } else if (strcmp(yylval.field, "attenuation") == 0) {
		field_repeat_test(&bitset, 1);
		get_token(REAL);
		coeff->scale = FROM_DB(-yylval.real);
		get_token(EOS);
	    } else if (strcmp(yylval.field, "filename") == 0) {
		field_repeat_test(&bitset, 2);
		if (parse_default) {
		    parse_error("cannot give coeff filename in default "
				"configuration.\n");
		}
		switch (token = yylex()) {
		case STRING:
		    strncpy(coeff->filename, yylval.string, PATH_MAX);
		    coeff->filename[PATH_MAX-1] = '\0';
		    get_token(EOS);
		    break;
		case REAL:
		    coeff->filename[0] = '\0';
		    n = 0;
		    do {
			coeff->shm_shmids[n] = make_integer(yylval.real);
			get_token(SLASH);
			get_token(REAL);
			coeff->shm_offsets[n] = make_integer(yylval.real);
			get_token(SLASH);
			get_token(REAL);
			coeff->shm_blocks[n] = make_integer(yylval.real);
			n++;
			if (n == BF_MAXCOEFFPARTS) {
			    parse_error("too many shared memory blocks.\n");
			}
			if ((token = yylex()) == COMMA) {
			    get_token(REAL);
			}
		    } while (token == COMMA);
		    if (token != EOS) {
			unexpected_token(EOS, token);
		    }
		    coeff->shm_elements = n;
		    break;
		default:
		    unexpected_token(STRING, token);
		    break;
		}
	    } else if (strcmp(yylval.field, "blocks") == 0) {
		field_repeat_test(&bitset, 3);
		get_token(REAL);
		coeff->coeff.n_blocks = make_integer(yylval.real);
		get_token(EOS);
	    } else if (strcmp(yylval.field, "shared_mem") == 0) {
		field_repeat_test(&bitset, 4);
		get_token(BOOLEAN);
		coeff->coeff.is_shared = yylval.boolean;
		get_token(EOS);
	    } else if (strcmp(yylval.field, "skip") == 0) {
		field_repeat_test(&bitset, 5);
		get_token(REAL);
		coeff->skip = make_integer(yylval.real);
		get_token(EOS);
	    } else {
		unrecognised_token("coeff field", yylval.field);
	    }
	    break;
	case RBRACE:
	    break;
	default:
	    unexpected_token(FIELD, token);
	}
    } while (token != RBRACE);
    get_token(EOS);
    
    if (parse_default) {
	field_mandatory_test(bitset, 0x1B, "coeff");
    } else {
        if (!has_defaults) {
            if (strcmp(coeff->filename, "dirac pulse") == 0) {
                if (!bit_isset(&bitset, 0)) {
                    coeff->format = COEFF_FORMAT_PROCESSED;
                }
                field_mandatory_test(bitset, 0x04, "coeff");
            } else {
                field_mandatory_test(bitset, 0x05, "coeff");
            }
        } else {
            field_mandatory_test(bitset, 0x04, "coeff");
        }
    }    

    if (coeff->format == COEFF_FORMAT_PROCESSED) {
	if (coeff->scale != 1.0) {
	    parse_error("cannot have non-zero attenuation on processed "
			"format.\n");
	}
    }    
    if (coeff->shm_elements > 0 && coeff->format != COEFF_FORMAT_PROCESSED) {
	parse_error("shared memory coefficients must be in processed "
		    "format.\n");
    }
    if (!parse_default && coeff->shm_elements > 0) {
        coeff->coeff.is_shared = true;
    }    
    return coeff;
}

static void
parse_filter_io_array(struct filter *filter,
		      bool_t parse_default,
		      int io,
		      bool_t isfilter)
{
    int len = 0, token;
    char name[BF_MAXOBJECTNAME];
    int io_array[BF_MAXCHANNELS];
    char msg[200];

    if (parse_default) {
	sprintf(msg, "cannot give filter %s in default configuration.\n",
		(io == IN) ? "inputs" : "outputs"); 
	parse_error(msg);
    }
    
    do {
	if (len == BF_MAXCHANNELS) {
	    parse_error("array is too long.\n");
	}
	if (!get_string_or_int(name, BF_MAXOBJECTNAME, &io_array[len])) {
	    io_array[len] = 0;
	    if (isfilter) {
		filter->filter_name[io][len] = estrdup(name);
	    } else {
		filter->channel_name[io][len] = estrdup(name);
	    }
	} else {
	    if (isfilter) {
                filter->filter_name[io][len] = NULL;
            } else {
                filter->channel_name[io][len] = NULL;            
            }
        }
        if (isfilter) {
            if (io == IN) {
                filter->fctrl.fscale[len] = 1.0;
            }
        } else {
            filter->fctrl.scale[io][len] = 1.0;
        }
	switch (token = yylex()) {
	case SLASH:
	    if (io == OUT && isfilter) {
		parse_error("cannot scale filter outputs which are connected "
			    "to other filter inputs.\n");
	    }
            switch (token = yylex()) {
            case SLASH:
                goto parse_scalar;
            case REAL:
                if (isfilter) {
                    filter->fctrl.fscale[len] *= FROM_DB(-yylval.real);
                } else {
                    filter->fctrl.scale[io][len] *= FROM_DB(-yylval.real);
                }
                switch (token = yylex()) {
                case EOS:
                case COMMA:
                    break;
                case SLASH:
                parse_scalar:
                    get_token(REAL);
                    if (isfilter) {
                        filter->fctrl.fscale[len] *= yylval.real;
                    } else {
                        filter->fctrl.scale[io][len] *= yylval.real;
                    }
                    switch (token = yylex()) {
                    case EOS:
                    case COMMA:
                        break;
                    default:
                        unexpected_token(EOS, token);
                    }
                    break;
                }
                break;
	    default:
		unexpected_token(REAL, token);
            }
	    break;	    
	case EOS:
	case COMMA:
	    break;
	default:
	    unexpected_token(EOS, token);
	}
	len++;
    } while (token != EOS);

    if (isfilter) {
	filter->filter.n_filters[io] = len;
	filter->filter.filters[io] = emalloc(len * sizeof(int));
	memcpy(filter->filter.filters[io], io_array, len * sizeof(int));
    } else {
	filter->filter.n_channels[io] = len;
	filter->filter.channels[io] = emalloc(len * sizeof(int));
	memcpy(filter->filter.channels[io], io_array, len * sizeof(int));
    }
}

static struct filter *
parse_filter(bool_t parse_default,
	     int intname)
{
    struct filter *filter;
    uint32_t bitset = 0;
    char msg[200];
    int token;

    filter = emalloc(sizeof(struct filter));    
    if (!parse_default) {
        if (has_defaults) {
            memcpy(filter, default_filter, sizeof(struct filter));
        } else {
            memset(filter, 0, sizeof(struct filter));            
            filter->fctrl.coeff = -1;
            filter->process = -1;
        }
	if (get_string_or_int(filter->filter.name, BF_MAXOBJECTNAME,
			      &filter->filter.intname))
	{
	    if (filter->filter.intname != intname) {
		parse_error("incorrect integer name.\n");
	    }
	    sprintf(filter->filter.name, "%d", intname);
	}
	filter->filter.intname = intname;
    } else {
	memset(filter, 0, sizeof(struct filter));
	filter->fctrl.coeff = -1;
        filter->process = -1;
    }

    get_token(LBRACE);
    
    do {
	switch (token = yylex()) {
	case FIELD:
	    if (strcmp(yylval.field, "process") == 0) {
		field_repeat_test(&bitset, 0);
		get_token(REAL);
		filter->process = make_integer(yylval.real);
		if (filter->process >= BF_MAXPROCESSES) {
		    sprintf(msg, "process is less than 0 "
			    "or larger than %d.\n", BF_MAXPROCESSES - 1);
		    parse_error(msg);
		}
                if (filter->process < 0) {
                    filter->process = -1;
                }
		get_token(EOS);
	    } else if (strcmp(yylval.field, "coeff") == 0) {
		field_repeat_test(&bitset, 1);
		if (parse_default) {
		    parse_error("cannot give filter coeff in default "
				"configuration.\n");
		}
		get_string_or_int(filter->coeff_name, BF_MAXOBJECTNAME,
				  &filter->fctrl.coeff);
		get_token(EOS);
	    } else if (strcmp(yylval.field, "from_inputs") == 0 ||
		       strcmp(yylval.field, "inputs") == 0)
	    {
		field_repeat_test(&bitset, 2);
		parse_filter_io_array(filter, parse_default, IN, false);
	    } else if (strcmp(yylval.field, "to_outputs") == 0 ||
		       strcmp(yylval.field, "outputs") == 0)
	    {
		field_repeat_test(&bitset, 3);
		parse_filter_io_array(filter, parse_default, OUT, false);
	    } else if (strcmp(yylval.field, "from_filters") == 0) {
		field_repeat_test(&bitset, 4);
		parse_filter_io_array(filter, parse_default, IN, true);
	    } else if (strcmp(yylval.field, "to_filters") == 0) {
		field_repeat_test(&bitset, 5);
		parse_filter_io_array(filter, parse_default, OUT, true);
	    } else if (strcmp(yylval.field, "delay") == 0) {
		field_repeat_test(&bitset, 6);
		get_token(REAL);
		filter->fctrl.delayblocks = make_integer(yylval.real);
		if (filter->fctrl.delayblocks < 0) {
		    filter->fctrl.delayblocks = 0;
		}
		/* dividing with block size when all configration has be read */
		get_token(EOS);
	    } else if (strcmp(yylval.field, "crossfade") == 0) {
		field_repeat_test(&bitset, 7);
		get_token(BOOLEAN);
		filter->filter.crossfade = yylval.boolean;
		get_token(EOS);
	    } else {
		unrecognised_token("filter field", yylval.field);
	    }
	    break;
	case RBRACE:
	    break;
	default:
	    unexpected_token(FIELD, token);
	}
    } while (token != RBRACE);
    get_token(EOS);
    
    if (!parse_default) {
	if (!bit_isset(&bitset, 5) && !bit_isset(&bitset, 3)) {
	    parse_error("no outputs for filter.\n");
	}
	if (!bit_isset(&bitset, 4) && !bit_isset(&bitset, 2)) {
	    parse_error("no inputs for filter.\n");
	}
	field_mandatory_test(bitset, 0x2, "filter");
    }

    /* some sanity checks and completion of inputs, outputs and coeff fields
       cannot be done until whole configuration has been read */

    return filter;
}

static struct iodev *
parse_iodev(bool_t parse_default,
	    int io,
	    int physical_intname_base,
	    int virtual_intname_base,
            int *maxdelay)
{
    struct iodev *iodev;
    int token, n, io_array[BF_MAXCHANNELS];
    char name[BF_MAXOBJECTNAME];
    uint32_t bitset = 0, inuse[BF_MAXCHANNELS / 32 + 1];
    int maxdelay_setting = -2, indmaxd_count = 0;

    iodev = emalloc(sizeof(struct iodev));
    if (!parse_default) {
        if (has_defaults) {
            memcpy(iodev, default_iodev[io], sizeof(struct iodev));
            iodev->device_params = default_iodev[io]->device_params;
            iodev->ch.channel_selection =
                emalloc(default_iodev[io]->ch.used_channels * sizeof(int));
            memcpy(iodev->ch.channel_selection,
                   default_iodev[io]->ch.channel_selection,
                   default_iodev[io]->ch.used_channels * sizeof(int));
            iodev->ch.channel_name =
                emalloc(default_iodev[io]->ch.used_channels * sizeof(int));
            for (n = 0; n < default_iodev[io]->ch.used_channels; n++) {
                iodev->ch.channel_name[n] = physical_intname_base + n;
            }
        } else {
            memset(iodev, 0, sizeof(struct iodev));
        }
	if (get_string_or_int(name, BF_MAXOBJECTNAME, &n)) {
	    iodev->virtual_channels = get_integer_list((double)n, io_array,
						       BF_MAXCHANNELS, LBRACE);
	    memcpy(iodev->channel_intname, io_array,
		   iodev->virtual_channels * sizeof(int));
	    for (n = 0; n < iodev->virtual_channels; n++) {
		if (iodev->channel_intname[n] != virtual_intname_base + n) {
		    parse_error("incorrect integer name.\n");
		}
		iodev->virt2phys[n] = n;
	    }
	    for (n = 0; n < iodev->virtual_channels; n++) {
		sprintf(name, "%d", iodev->channel_intname[n]);
		iodev->channel_name[n] = estrdup(name);
	    }
	} else {
	    iodev->virtual_channels = get_string_list(name, iodev->channel_name,
						      BF_MAXCHANNELS, LBRACE);
	    for (n = 0; n < iodev->virtual_channels; n++) {
		iodev->channel_intname[n] = virtual_intname_base + n;
		iodev->virt2phys[n] = n;
	    }
	}
    } else {
	memset(iodev, 0, sizeof(struct iodev));
	get_token(LBRACE);
    }
    
    do {
	switch (token = yylex()) {
	case FIELD:
	    if (strcmp(yylval.field, "device") == 0) {
		field_repeat_test(&bitset, 0);
		get_token(STRING);
		strncpy(iodev->device_name, yylval.string, BF_MAXOBJECTNAME);
		iodev->device_name[BF_MAXOBJECTNAME - 1] = '\0';
		if (strchr(iodev->device_name, PATH_SEPARATOR_CHAR) != NULL) {
		    parse_error("path separator not allowed in device name.\n");
		}
		iodev->device_params = get_params();
		get_token(EOS);
	    } else if (strcmp(yylval.field, "sample") == 0) {
		field_repeat_test(&bitset, 1);
		get_token(STRING);
		iodev->auto_format =
                    parse_sample_format(&iodev->ch.sf, yylval.string, true);
		get_token(EOS);
	    } else if (strcmp(yylval.field, "channels") == 0) {
		field_repeat_test(&bitset, 2);
		get_token(REAL);
		iodev->ch.open_channels = make_integer(yylval.real);
		if (iodev->ch.open_channels < 1 ||
		    iodev->ch.open_channels > BF_MAXCHANNELS)
		{
		    parse_error("too few or too many channels.\n");
		}
		if (iodev->ch.channel_selection != NULL) {
		    efree(iodev->ch.channel_selection);
		}
		if (iodev->ch.channel_name != NULL) {
		    efree(iodev->ch.channel_name);
		}
		switch (token = yylex()) {
		case SLASH:
		    get_token(REAL);
		    n = get_integer_list(yylval.real, io_array,
					 BF_MAXCHANNELS, EOS);
		    iodev->ch.used_channels = n;
		    if (n > iodev->ch.open_channels) {
			parse_error("channel amount mismatch.\n");
		    }
		    iodev->ch.channel_selection = emalloc(n * sizeof(int));
		    memcpy(iodev->ch.channel_selection, io_array,
			   n * sizeof(int));
		    memset(inuse, 0, sizeof(inuse));
		    for (n = 0; n < iodev->ch.used_channels; n++) {
			if (iodev->ch.channel_selection[n] < 0 ||
			    iodev->ch.channel_selection[n] >=
			    iodev->ch.open_channels)
			{
			    parse_error("channel out of range.\n");
			}
			if (bit_isset(inuse, iodev->ch.channel_selection[n])) {
			    parse_error("duplicate channel selection.\n");
			}
			bit_set(inuse, iodev->ch.channel_selection[n]);
		    }
		    break;
		case EOS:
		    iodev->ch.used_channels = iodev->ch.open_channels;
		    iodev->ch.channel_selection =
			emalloc(iodev->ch.used_channels * sizeof(int));
		    for (n = 0; n < iodev->ch.used_channels; n++) {
			iodev->ch.channel_selection[n] = n;
		    }
		    break;
		default:
		    unexpected_token(EOS, token);
		}
		if (!parse_default &&
		    iodev->ch.used_channels > iodev->virtual_channels)
		{
		    parse_error("channel amount exceeds allocated.\n");
		}
		iodev->ch.channel_name =
		    emalloc(iodev->ch.used_channels * sizeof(int));
		for (n = 0; n < iodev->ch.used_channels; n++) {
		    iodev->ch.channel_name[n] = physical_intname_base + n;
		}
	    } else if (strcmp(yylval.field, "delay") == 0) {
		field_repeat_test(&bitset, 3);
		get_token(REAL);
		n = get_integer_list(yylval.real,
				     &bfconf->delay[io][virtual_intname_base],
				     BF_MAXCHANNELS - virtual_intname_base,
				     EOS);
		while (n--) {
		    if (bfconf->delay[io][virtual_intname_base + n] < 0) {
			parse_error("negative delay.\n");
		    }
                    if (bfconf->delay[io][virtual_intname_base + n] >
                        *maxdelay)
                    {
                        *maxdelay =
                            bfconf->delay[io][virtual_intname_base + n];
                    }
		}
	    } else if (strcmp(yylval.field, "dither") == 0) {
		field_repeat_test(&bitset, 4);
		if (io == IN) {
		    unrecognised_token("input field", yylval.field);
		}
		get_token(BOOLEAN);
		iodev->apply_dither = yylval.boolean;
		get_token(EOS);
	    } else if (strcmp(yylval.field, "mute") == 0) {
		field_repeat_test(&bitset, 5);
		for (n = virtual_intname_base; n < BF_MAXCHANNELS; n++) {
		    get_token(BOOLEAN);
		    bfconf->mute[io][n] = yylval.boolean;
		    if ((token = yylex()) == COMMA) {
			continue;
		    } else if (token == EOS) {
			break;
		    } else {
			unexpected_token(EOS, token);
		    }
		}
	    } else if (strcmp(yylval.field, "maxdelay") == 0) {
		field_repeat_test(&bitset, 6);
		get_token(REAL);
		maxdelay_setting = make_integer(yylval.real);
                if (maxdelay_setting > *maxdelay) {
                    *maxdelay = maxdelay_setting;
                }
                if (maxdelay_setting < 0) {
                    maxdelay_setting = -1;
                }
		get_token(EOS);
	    } else if (strcmp(yylval.field, "individual_maxdelay") == 0) {
		field_repeat_test(&bitset, 7);
		get_token(REAL);
		n = get_integer_list(yylval.real,
				     &bfconf->maxdelay[io]
                                     [virtual_intname_base],
				     BF_MAXCHANNELS - virtual_intname_base,
				     EOS);
                indmaxd_count = n;
		while (n--) {
		    if (bfconf->maxdelay[io][virtual_intname_base + n] < 0) {
                        bfconf->maxdelay[io][virtual_intname_base + n] = -1;
		    }
                    if (bfconf->maxdelay[io][virtual_intname_base + n] >
                        *maxdelay)
                    {
                        *maxdelay =
                            bfconf->maxdelay[io][virtual_intname_base + n];
                    }
		}
	    } else if (strcmp(yylval.field, "mapping") == 0) {		
		field_repeat_test(&bitset, 8);
		if (parse_default) {
		    unrecognised_token("default io device field", yylval.field);
		}
		get_token(REAL);
		n = get_integer_list(yylval.real, iodev->virt2phys,
				     BF_MAXCHANNELS, EOS);
		if (n != iodev->virtual_channels) {
		    parse_error("channel amount mismatch.\n");
		}
	    } else if (strcmp(yylval.field, "merge") == 0) {
		field_repeat_test(&bitset, 9);
                pinfo("Warning: \"merge\" setting is deprecated.\n");
		if (io == IN) {
		    unrecognised_token("input field", yylval.field);
		}
		get_token(BOOLEAN);
		get_token(EOS);
	    } else if (strcmp(yylval.field, "subdelay") == 0) {
		field_repeat_test(&bitset, 10);
		get_token(REAL);
		n = get_integer_list(yylval.real,
				     &bfconf->subdelay[io]
                                     [virtual_intname_base],
				     BF_MAXCHANNELS - virtual_intname_base,
				     EOS);
		while (n--) {
		    if (bfconf->subdelay[io][virtual_intname_base + n] <=
                        -BF_SAMPLE_SLOTS)
                    {
                        bfconf->subdelay[io][virtual_intname_base + n] =
                            BF_UNDEFINED_SUBDELAY;
		    } else {
                        bfconf->use_subdelay[io] = true;
                    }
                    if (bfconf->subdelay[io][virtual_intname_base + n] >=
                        BF_SAMPLE_SLOTS)
                    {
			parse_error("too large subdelay.\n");
                    }
		}
	    } else {
		unrecognised_token((io == IN) ? "input field" : "output filed",
				   yylval.field);
	    }
	    break;
	case RBRACE:
	    break;
	default:
	    unexpected_token(FIELD, token);
	    return NULL;
	}
    } while (token != RBRACE);
    get_token(EOS);
    
    if (io == IN) {
	if (parse_default || !has_defaults) {
	    field_mandatory_test(bitset, 0x07, "input");
	}
    } else {
	if (parse_default || !has_defaults) {
	    field_mandatory_test(bitset, 0x07, "output");
	}
    }
    if (!parse_default) {

        if (maxdelay_setting != -2) {
            for (n = indmaxd_count; n < iodev->virtual_channels; n++) {
                bfconf->maxdelay[io][virtual_intname_base + n] =
                    maxdelay_setting;
            }
        }
        
	for (n = 0; n < iodev->virtual_channels; n++) {
	    if (iodev->virt2phys[n] < 0 ||
		iodev->virt2phys[n] >= iodev->ch.used_channels)
	    {
		parse_error("invalid channel mapping.\n");
	    }
	}	
	if (bit_isset(&bitset, 8) &&
	    iodev->virtual_channels <= iodev->ch.used_channels)
	{
	    parse_error("virtual mapping only allowed when virtual channel "
			"amount exceeds physical.\n");
	}
    }
    for (n = 0; n < iodev->virtual_channels; n++) {
        if (bfconf->maxdelay[io][virtual_intname_base + n] >= 0 &&
            bfconf->delay[io][virtual_intname_base + n] >
            bfconf->maxdelay[io][virtual_intname_base + n])
        {
            parse_error("delay exceeds specified maximum delay.\n");
        }
    }
    
    return iodev;
}

static void
parse_setting(char field[],
	      bool_t parse_default,
	      uint32_t *repeat_bitset)
{
    char msg[200];
    int token, n;    

    if (strcmp(field, "sampling_rate") == 0) {
	field_repeat_test(repeat_bitset, 0);
	get_token(REAL);
	bfconf->sampling_rate = make_integer(yylval.real);
	if (bfconf->sampling_rate <= 0) {
	    parse_error("invalid sampling_rate.\n");
	}
	get_token(EOS);
    } else if (strcmp(field, "config_file") == 0) {
	field_repeat_test(repeat_bitset, 1);
	if (!parse_default) {
	    parse_error("cannot set config_file setting in this file.\n");
	}
	get_token(STRING);
	strcpy(default_config_file, tilde_expansion(yylval.string));
	get_token(EOS);
    } else if (strcmp(field, "logic") == 0) {
	field_repeat_test(repeat_bitset, 2);
	do {
	    get_token(STRING);
	    for (n = 0; n < bfconf->n_logicmods; n++) {
		if (strcmp(yylval.string, logic_names[n]) == 0)	{
		    pinfo("Warning: overriding parameters for module \"%s\".\n",
			  yylval.string);
		    efree(logic_names[n]);
		    free_params(logic_params[n]);
		    break;
		}
	    }
	    logic_names[n] = estrdup(yylval.string);
	    if (strchr(yylval.string, PATH_SEPARATOR_CHAR) != NULL) {
		parse_error("path separator not allowed in module name.\n");
	    }
            logic_params[n] = get_params();
            token = yylex();
	    switch (token) {
	    case COMMA:
	    case EOS:
		if (n == bfconf->n_logicmods) {
		    if (++bfconf->n_logicmods == BF_MAXMODULES) {
			parse_error("too many modules.\n");
		    }
		}
		break;
	    default:
		unexpected_token(EOS, token);
		break;
	    }
	} while (token != EOS);
    } else if (strcmp(field, "overflow_warnings") == 0) {
	field_repeat_test(repeat_bitset, 3);
	get_token(BOOLEAN);
	bfconf->overflow_warnings = yylval.boolean;
	get_token(EOS);
    } else if (strcmp(field, "show_progress") == 0) {
	field_repeat_test(repeat_bitset, 4);
	get_token(BOOLEAN);
	bfconf->show_progress = yylval.boolean;
	get_token(EOS);
    } else if (strcmp(field, "n_processors") == 0) {
	field_repeat_test(repeat_bitset, 5);
        pinfo("Warning: \"n_processors\" setting is deprecated.\n");
	get_token(REAL);
	n = make_integer(yylval.real);
	if (n < 1) {
	    parse_error("invalid number of processors.\n");
	}	
	get_token(EOS);
    } else if (strcmp(field, "max_dither_table_size") == 0) {
	field_repeat_test(repeat_bitset, 6);
	get_token(REAL);
	bfconf->max_dither_table_size = make_integer(yylval.real);
	get_token(EOS);
    } else if (strcmp(field, "filter_length") == 0) {
	field_repeat_test(repeat_bitset, 7);
	get_token(REAL);
	bfconf->filter_length = make_integer(yylval.real);
	switch (token = yylex()) {
	case EOS:
	    bfconf->n_blocks = 1;
	    break;
	case COMMA:
	    get_token(REAL);
	    bfconf->n_blocks = make_integer(yylval.real);
	    get_token(EOS);
	    break;
	default:
	    unexpected_token(EOS, token);
	    break;
	}
	if (log2_get(bfconf->filter_length) == -1 ||
	    bfconf->n_blocks * bfconf->filter_length < MINFILTERLEN ||
	    bfconf->n_blocks * bfconf->filter_length > MAXFILTERLEN)
	{
	    sprintf(msg, "filter length is not within %d - %d "
		    "or not a power of 2.\n",
		    MINFILTERLEN, MAXFILTERLEN);
	    parse_error(msg);
	}
    } else if (strcmp(field, "lock_memory") == 0) {
	field_repeat_test(repeat_bitset, 8);
	get_token(BOOLEAN);
	bfconf->lock_memory = yylval.boolean;
	get_token(EOS);
    } else if (strcmp(field, "modules_path") == 0) {
	field_repeat_test(repeat_bitset, 9);
	get_token(STRING);
	if (modules_path == NULL) {
	    modules_path = emalloc(PATH_MAX + 1);
	}
	strcpy(modules_path, tilde_expansion(yylval.string));
	n = strlen(modules_path);
	if (modules_path[n - 1] != PATH_SEPARATOR_CHAR) {
	    modules_path[n] = PATH_SEPARATOR_CHAR;
	    modules_path[n + 1] = '\0';
	}
	get_token(EOS);
    } else if (strcmp(field, "monitor_rate") == 0) {
	field_repeat_test(repeat_bitset, 10);
	get_token(BOOLEAN);
	bfconf->monitor_rate = yylval.boolean;
	get_token(EOS);
    } else if (strcmp(field, "debug") == 0) {
	field_repeat_test(repeat_bitset, 11);
	get_token(BOOLEAN);
	bfconf->debug = yylval.boolean;
	get_token(EOS);
    } else if (strcmp(field, "powersave") == 0) {
	field_repeat_test(repeat_bitset, 12);
	switch (token = yylex()) {
	case REAL:
            bfconf->analog_powersave = FROM_DB(yylval.real);
            if (bfconf->analog_powersave < 1.0) {
                bfconf->powersave = true;
            }
	    break;
	case BOOLEAN:
            bfconf->analog_powersave = 1.0;
            bfconf->powersave = yylval.boolean;
	    break;
	default:
	    unexpected_token(BOOLEAN, token);
	    break;
	}
        get_token(EOS);
    } else if (strcmp(field, "allow_poll_mode") == 0) {
	field_repeat_test(repeat_bitset, 13);
	get_token(BOOLEAN);
	bfconf->allow_poll_mode = yylval.boolean;
	get_token(EOS);
    } else if (strcmp(field, "float_bits") == 0) {
	field_repeat_test(repeat_bitset, 14);
	get_token(REAL);
	bfconf->realsize = make_integer(yylval.real);
        if (bfconf->realsize != sizeof(float) * 8 &&
            bfconf->realsize != sizeof(double) * 8)
        {
            sprintf(msg, "invalid float_bits, must be %zd or %zd.\n",
                    sizeof(float) * 8, sizeof(double) * 8); 
            parse_error(msg);
        }
        bfconf->realsize /= 8;
	get_token(EOS);
#ifdef CONVOLVER_NEEDS_CONFIGFILE
    } else if (strcmp(field, "convolver_config") == 0) {
	field_repeat_test(repeat_bitset, 15);
	get_token(STRING);
	if (convolver_config == NULL) {
	    convolver_config = emalloc(PATH_MAX);
	}
	strcpy(convolver_config, tilde_expansion(yylval.string));
	get_token(EOS);
#endif	
    } else if (strcmp(field, "benchmark") == 0) {
	if (parse_default) {
	    parse_error("cannot set benchmark setting in this file.\n");
	}
	field_repeat_test(repeat_bitset, 16);
	get_token(BOOLEAN);
	bfconf->benchmark = yylval.boolean;
	get_token(EOS);
        if (has_defaults && bfconf->benchmark) {
            fprintf(stderr, "The benchmark option requires the "
                    "\"-nodefault\" switch.\n");
            exit(BF_EXIT_INVALID_CONFIG);
        }
    } else if (strcmp(field, "sdf_length") == 0) {
	field_repeat_test(repeat_bitset, 17);
	get_token(REAL);
	bfconf->sdf_length = make_integer(yylval.real);
        if (bfconf->sdf_length <= 0) {
            bfconf->sdf_length = -1;
        }
	switch (token = yylex()) {
	case EOS:
	    bfconf->sdf_beta = 9.0;
	    break;
	case COMMA:
	    get_token(REAL);
	    bfconf->sdf_beta = yylval.real;
	    get_token(EOS);
	    break;
	default:
	    unexpected_token(EOS, token);
	    break;
	}
    } else if (strcmp(field, "safety_limit") == 0) {
	field_repeat_test(repeat_bitset, 18);
	get_token(REAL);
	bfconf->safety_limit = pow(10.0, yylval.real / 20.0);
        if (!isfinite(bfconf->safety_limit)) {
            fprintf(stderr, "invalid safety_limit.\n");
            exit(BF_EXIT_INVALID_CONFIG);
        }
	get_token(EOS);
    } else {
	parse_error("unrecognised setting name.\n");
    }
}

static void
get_defaults(void)
{
    struct stat filestat;
    uint32_t repeat_bitset = 0;
#ifdef CONVOLVER_NEEDS_CONFIGFILE
    uint32_t bits = 0x85DB;
#else
    uint32_t bits = 0x05DB;
#endif
    int token, io, dummy;

    strcpy(current_filename, tilde_expansion(DEFAULT_BFCONF_NAME));
    
    if (stat(current_filename, &filestat) != 0) {
	create_default_config();
    }

    if ((yyin = fopen(current_filename, "rt")) == NULL) {
	fprintf(stderr, "Could not open file \"" DEFAULT_BFCONF_NAME
		"\" for reading.\n");
	exit(BF_EXIT_OTHER);
    }

    do {
	switch (token = yylex()) {
	case FIELD:
	    parse_setting(yylval.field, true, &repeat_bitset);
	    break;
	case COEFF:
	    if (default_coeff != NULL) {
		fprintf(stderr, "More than one coeff structure in "
			"default configuration.\n");
		exit(BF_EXIT_INVALID_CONFIG);
	    }
	    default_coeff = parse_coeff(true, 0);
	    break;
	case INPUT:
	case OUTPUT:
	    io = (token == INPUT) ? IN : OUT;
	    if (default_iodev[io] != NULL) {
		fprintf(stderr, "More than one %s structure in "
			"default configuration.\n",
			(io == IN) ? "input" : "output");
		exit(BF_EXIT_INVALID_CONFIG);
	    }
            dummy = 0;
	    default_iodev[io] = parse_iodev(true, io, 0, 0, &dummy);
	    break;
	case FILTER:
	    if (default_filter != NULL) {
		fprintf(stderr, "More than one filter structure in "
			"default configuration.\n");
		exit(BF_EXIT_INVALID_CONFIG);
	    }
	    default_filter = parse_filter(true, 0);
	    break;
	case EOF:
	    break;
	default:
	    unexpected_token(FIELD, token);
	}
    } while (token != EOF);
    fclose(yyin);

    FOR_IN_AND_OUT {
	if (default_iodev[IO] == NULL) {
	    fprintf(stderr, "No %s defined in %s.\n",
		    (IO == IN) ? "input" : "output", current_filename);
	    exit(BF_EXIT_INVALID_CONFIG);
	}
    }
    if (default_filter == NULL) {
        default_filter = emalloc(sizeof(struct filter));
        memset(default_filter, 0, sizeof(struct filter));
    }
    if (default_coeff == NULL) {
	fprintf(stderr, "No coeff defined in %s.\n", current_filename);
	exit(BF_EXIT_INVALID_CONFIG);
    }
    field_mandatory_test(repeat_bitset, bits, current_filename);
}

static void *
real_read(FILE *stream,
          int *len,
          char filename[],
          int realsize,
          int maxitems)
{
    char str[1024], *p, *s;
    void *realbuf = NULL;
    int capacity = 0;
    
    *len = 0;

    str[1023] = '\0';
    while (fgets(str, 1023, stream) != NULL) {
	s = str;
	while (*s == ' ' || *s == '\t') s++;
	if (*s == '\n' || *s == '\0') {
	    continue;
	}
	if (*len == capacity) {
	    capacity += 1024;
	    realbuf = erealloc(realbuf, capacity * realsize);
	}
        if (realsize == 4) {
            ((float *)realbuf)[*len] = (float)strtod(s, &p);
        } else {
            ((double *)realbuf)[*len] = strtod(s, &p);
        }
	(*len) += 1;
	if (p == s) {
	    fprintf(stderr, "Parse error on line %d in file %s: invalid "
		    "floating point number.\n", *len, filename);
	    exit(BF_EXIT_INVALID_CONFIG);
	}
        if (maxitems > 0 && (*len) == maxitems) {
            break;
        }
    }
    realbuf = erealloc(realbuf, (*len) * realsize);
    return realbuf;
}

#define REALSIZE 4
#define RAW2REAL_NAME raw2realf
#include "raw2real.h"
#undef REALSIZE
#undef RAW2REAL_NAME

#define REALSIZE 8
#define RAW2REAL_NAME raw2reald
#include "raw2real.h"
#undef REALSIZE
#undef RAW2REAL_NAME

static void *
raw_read(FILE *stream,
	 int *totitems,
	 struct sample_format *sf,
         int realsize,
         int maxitems)
{
    int n_items;
    uint8_t *rawbuf = NULL, *curp = NULL;
    void *realbuf;

    *totitems = 0;
    curp = rawbuf = emalloc(1024 * sf->bytes);
    while ((n_items = fread(curp, sf->bytes, 1024, stream)) != 0) {
	*totitems += n_items;
        if (maxitems > 0 && *totitems >= maxitems) {
            *totitems = maxitems;
            break;
        }
	rawbuf = erealloc(rawbuf, (*totitems + 1024) * sf->bytes);
	curp = rawbuf + (*totitems) * sf->bytes;
    }
    rawbuf = erealloc(rawbuf, (*totitems) * sf->bytes);
    if (sf->isfloat && !sf->swap && sf->bytes == realsize) {
	return (void *)rawbuf;
    }
    realbuf = emalloc((*totitems) * realsize);
    if (realsize == 4) {
        raw2realf(realbuf, rawbuf, sf->bytes,
                  sf->isfloat, 1, sf->swap, *totitems);
        for (n_items = 0; n_items < *totitems; n_items++) {
            ((float *)realbuf)[n_items] *= (float)sf->scale;
        }
    } else {
        raw2reald(realbuf, rawbuf, sf->bytes,
                  sf->isfloat, 1, sf->swap, *totitems);
        for (n_items = 0; n_items < *totitems; n_items++) {
            ((double *)realbuf)[n_items] *= sf->scale;
        }
    }
    efree(rawbuf);
    return realbuf;
}

static void *
get_sharedmem(int shmid,
	      int offset)
{
    static void **shmsegs = NULL;
    static int *shmid_array = NULL;
    static int n_shmids = 0;
    static int cap = 0;
    void *buf, *p;
    int n;
    
    buf = NULL;
    for (n = 0; n < n_shmids; n++) {
	if (shmid == shmid_array[n]) {
	    buf = shmsegs[n];
	    break;
	}
    }
    if (n == n_shmids) {
	if ((buf = shmat(shmid, NULL, 0)) == (void *)-1) {
	    fprintf(stderr, "Failed to attach to shared memory with id %d: "
		    "%s.\n", shmid, strerror(errno));
	    exit(BF_EXIT_OTHER);
	}
	if (n_shmids == cap) {
	    cap += 16;
	    shmsegs = erealloc(shmsegs, cap * sizeof (void *));
	    shmid_array = erealloc(shmid_array, cap * sizeof (int));
	}
	shmid_array[n_shmids] = shmid;
	shmsegs[n_shmids] = buf;
	n_shmids++;
    }
    p = &((uint8_t *)buf)[offset];
    if (((ptrdiff_t)p & (ALIGNMENT - 1)) != 0) {
        fprintf(stderr, "Shared memory pointer with id %d and offset %d "
                " is not aligned at a %d byte boundary.\n", shmid, offset,
                ALIGNMENT);
        exit(BF_EXIT_OTHER);
    }
    return (void *)&((uint8_t *)buf)[offset];
}

static void *
load_coeff(struct coeff *coeff,
           int cindex,
           int realsize)
{
    void *coeffs, *zbuf = NULL;
    FILE *stream = NULL;
    void **cbuf, *buf;
    int n, i, j, len;
    uint8_t *dest;

    if (coeff->shm_elements <= 0 &&
	strcmp(coeff->filename, "dirac pulse") != 0)
    {
	if ((stream = fopen(coeff->filename,
			    coeff->format == COEFF_FORMAT_TEXT ? "rt" : "rb"))
	    == NULL)
	{
	    fprintf(stderr, "Could not open \"%s\" for reading.\n",
		    coeff->filename);
	    exit(BF_EXIT_OTHER);
	}
        if (coeff->skip > 0) {
            fseek(stream, coeff->skip, SEEK_SET);
            if (ftell(stream) != coeff->skip) {
                fprintf(stderr, "Failed to skip %d bytes of file \"%s\".\n",
                        coeff->skip, coeff->filename);
                exit(BF_EXIT_OTHER);
            }
        }
    } else if (coeff->skip > 0) {
        fprintf(stderr, "Cannot use skip field for coeff \"%s\" "
              "(skip only works on files).\n", coeff->coeff.name);
        exit(BF_EXIT_INVALID_CONFIG);
    }

    cbuf = emalloc(coeff->coeff.n_blocks * sizeof(void **));
    
    if (strcmp(coeff->filename, "dirac pulse") == 0) {
	len = coeff->coeff.n_blocks * bfconf->filter_length;
	coeffs = emalloc(len * realsize);
	memset(coeffs, 0, len * realsize);
        if (realsize == 4) {
            ((float *)coeffs)[0] = 1.0;
        } else {
            ((double *)coeffs)[0] = 1.0;
        }
    } else {
	switch (coeff->format) {
	case COEFF_FORMAT_TEXT:
	    coeffs = real_read(stream, &len, coeff->filename, realsize,
                               coeff->coeff.n_blocks * bfconf->filter_length);
	    break;
	case COEFF_FORMAT_RAW:
	    coeffs = raw_read(stream, &len, &coeff->rawformat, realsize,
                              coeff->coeff.n_blocks * bfconf->filter_length);
	    break;
	case COEFF_FORMAT_PROCESSED:
	    if (coeff->shm_elements > 0) {
		for (i = j = 0; i < coeff->shm_elements; i++) {
		    j += coeff->shm_blocks[i];
		}
		if (j != coeff->coeff.n_blocks) {
		    fprintf(stderr, "Shared memory block count mismatch in "
			    "coeff %d.\n", coeff->coeff.intname);
		    exit(BF_EXIT_INVALID_CONFIG);
		}
		for (i = j = 0; i < coeff->shm_elements; i++) {
		    buf = get_sharedmem(coeff->shm_shmids[i],
					coeff->shm_offsets[i]);
		    for (n = 0; n < coeff->shm_blocks[i]; n++) {
			cbuf[j++] =
                            (void *)&((uint8_t *)buf)[n * convolver_cbufsize()];
		    }
		}
	    } else {
		buf =
                    raw_read(stream, &len, &coeff->rawformat, realsize,
                             coeff->coeff.n_blocks * convolver_cbufsize() + 1);
		if (coeff->coeff.n_blocks * convolver_cbufsize() != len) {
		    fprintf(stderr, "Length mismatch of file \"%s\", expected "
			    "%d, got %d.\n",
			    coeff->filename,
			    coeff->coeff.n_blocks * convolver_cbufsize(), len);
		    exit(BF_EXIT_INVALID_CONFIG);
		}
		for (n = 0; n < coeff->coeff.n_blocks; n++) {
		    cbuf[n] =
                        (void *)&((uint8_t *)buf)[n * convolver_cbufsize()];
		}
	    }
            if (!convolver_verify_cbuf(cbuf, coeff->coeff.n_blocks)) {
                fprintf(stderr, "Coeff %d is invalid.\n", coeff->coeff.intname);
                exit(BF_EXIT_INVALID_CONFIG);
            }
#if 0            
            if (bfconf->debug) {
                char filename[1024];                
                sprintf(filename, "brutefir-%d-coeffs-%d.txt",
                        getpid(), cindex);
                convolver_debug_dump_cbuf(filename, cbuf,
                                          coeff->coeff.n_blocks);
            }
#endif            
	    return cbuf;
	default:
	    fprintf(stderr, "Invalid format: %d.\n", coeff->format);
	    exit(BF_EXIT_INVALID_CONFIG);
	}
	fclose(stream);
    }

    if (len < coeff->coeff.n_blocks * bfconf->filter_length) {
	zbuf = emalloc(bfconf->filter_length * realsize);
	memset(zbuf, 0, bfconf->filter_length * realsize);
    }
    if (coeff->coeff.is_shared) {
        dest = shmalloc(2 * coeff->coeff.n_blocks * bfconf->filter_length *
                        realsize);
        if (dest == NULL) {
            exit(BF_EXIT_NO_MEMORY);
        }
    } else {
        dest = NULL;
    }
    for (n = 0; n < coeff->coeff.n_blocks; n++) {
	if (n * bfconf->filter_length > len) {
	    cbuf[n] = convolver_coeffs2cbuf(zbuf,
					    bfconf->filter_length,
					    coeff->scale,
                                            dest);
	} else if ((n + 1) * bfconf->filter_length > len) {
	    cbuf[n] = convolver_coeffs2cbuf
                (&((uint8_t *)coeffs)[n * bfconf->filter_length * realsize],
                 len - n * bfconf->filter_length,
                 coeff->scale,
                 dest);
	} else {
	    cbuf[n] = convolver_coeffs2cbuf
                (&((uint8_t *)coeffs)[n * bfconf->filter_length * realsize],
                 bfconf->filter_length,
                 coeff->scale,
                 dest);
	}
	if (cbuf[n] == NULL) {
	    fprintf(stderr, "Failed to preprocess coefficients in file %s.\n",
		    coeff->filename);
	    exit(BF_EXIT_OTHER);
	}
        if (coeff->coeff.is_shared) {
            dest += 2 * bfconf->filter_length * realsize;
        }
    }
    efree(zbuf);
    efree(coeffs);
#if 0    
    if (bfconf->debug) {
        char filename[1024];                
        sprintf(filename, "brutefir-%d-coeffs-%d.txt", getpid(), cindex);
        convolver_debug_dump_cbuf(filename, cbuf, coeff->coeff.n_blocks);
    }
#endif    
    return cbuf;
}

static bool_t
filter_loop(int source_intname,
	    int search_intname)
{
    int n;
    
    for (n = 0; n < bfconf->filters[search_intname].n_filters[OUT]; n++) {
	if (bfconf->filters[search_intname].filters[OUT][n] ==
	    source_intname ||
	    filter_loop(source_intname,
			bfconf->filters[search_intname].filters[OUT][n]))
	{
	    return true;
	}
    }
    return false;
}

static void *
load_module_function(void *handle,
		     const char modname[],
		     const char name[],
		     bool_t required)
{
    char *funname;
    void *fun;

    funname = alloca(strlen(name) + 1);
    strcpy(funname, name);
    if ((fun = dlsym(handle, funname)) == NULL && required) {
	fprintf(stderr, "Failed to resolve function \"%s\" in module "
		"\"%s\": %s.\n", name, modname, dlerror());
	exit(BF_EXIT_OTHER);
    }
    return fun;
}

static void
find_module(char path[],
            const char name[],
            const char suffix[])
{
    struct stat filestat;
    
    if (modules_path != NULL) {
        sprintf(path, "%s%s%s", modules_path, name, suffix);
    }
    if (stat(path, &filestat) == 0) {
        return;
    }
    sprintf(path, "/usr/local/lib/brutefir/%s%s", name, suffix);
    if (stat(path, &filestat) == 0) {
        return;
    }
    sprintf(path, "/usr/local/lib/%s%s", name, suffix);
    if (stat(path, &filestat) == 0) {
        return;
    }
    sprintf(path, "/usr/lib/brutefir/%s%s", name, suffix);
    if (stat(path, &filestat) == 0) {
        return;
    }
    sprintf(path, "/usr/lib/%s%s", name, suffix);
    if (stat(path, &filestat) == 0) {
        return;
    }
    fprintf(stderr, "Failed to find module \"%s\". "
            "None of the following files existed:\n", name);
    if (modules_path != NULL) {
        fprintf(stderr, "  \"%s%s%s\"\n", modules_path, name, suffix);
    }
    fprintf(stderr, "  \"/usr/local/lib/brutefir/%s%s\"\n", name, suffix);
    fprintf(stderr, "  \"/usr/local/lib/%s%s\"\n", name, suffix);
    fprintf(stderr, "  \"/usr/lib/brutefir/%s%s\"\n", name, suffix);
    fprintf(stderr, "  \"/usr/lib/%s%s\"\n", name, suffix);
    exit(BF_EXIT_OTHER);
}

static void
load_bfio_module(struct bfio_module *m,
		 const char name[])
{    
    char path[BF_MAXOBJECTNAME + PATH_MAX + 100];
    int (*iscallback)(void);

    memset(m, 0, sizeof(struct bfio_module));
    find_module(path, name, ".bfio");
    if ((m->handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL)) == NULL) {
	fprintf(stderr, "Failed to open module \"%s\" in \"%s\": %s.\n",
		name, path, dlerror());
	exit(BF_EXIT_OTHER);
    }
    iscallback = load_module_function(m->handle, name, BF_FUN_BFIO_ISCALLBACK,
                                      false);
    if (iscallback != NULL) {
        m->iscallback = iscallback();
    } else {
        m->iscallback = false;
    }
    m->preinit = load_module_function(m->handle, name, BF_FUN_BFIO_PREINIT,
                                      true);
    m->command = load_module_function(m->handle, name, BF_FUN_BFIO_COMMAND,
                                      false);
    m->init = load_module_function(m->handle, name, BF_FUN_BFIO_INIT, true);
    m->read = load_module_function(m->handle, name, BF_FUN_BFIO_READ, false);
    m->write = load_module_function(m->handle, name, BF_FUN_BFIO_WRITE, false);
    m->synch_start = load_module_function(m->handle, name,
                                          BF_FUN_BFIO_SYNCH_START,
                                          m->iscallback);
    m->synch_stop = load_module_function(m->handle, name,
                                         BF_FUN_BFIO_SYNCH_STOP, m->iscallback);
    m->start = load_module_function(m->handle, name, BF_FUN_BFIO_START, false);
    m->stop = load_module_function(m->handle, name, BF_FUN_BFIO_STOP, false);
    m->message = load_module_function(m->handle, name, BF_FUN_BFIO_MESSAGE,
                                      false);
    if (!m->iscallback && m->read == NULL && m->write == NULL) {
	fprintf(stderr, "Module \"%s\" in \"%s\" lacks both read and write "
		"functions.\n", name, path);
	exit(BF_EXIT_OTHER);
    }
    if (m->iscallback && m->command != NULL) {
	fprintf(stderr, "Command function for callback I/O not supported.\n");
	exit(BF_EXIT_OTHER);
    }
    if ((m->synch_start != NULL && m->synch_stop == NULL) ||
        (m->synch_start == NULL && m->synch_stop != NULL) ||
        (m->start != NULL && m->stop == NULL) ||
        (m->start == NULL && m->stop != NULL) ||
        (m->start == NULL && m->synch_start == NULL))
    {
	fprintf(stderr, "Module \"%s\" in \"%s\" lacks start and/or stop "
		"functions.\n", name, path);
	exit(BF_EXIT_OTHER);        
    }
    if (m->command != NULL && m->message == NULL) {
        fprintf(stderr, "Module \"%s\" in \"%s\" has command function but "
                "lacks message function.\n", name, path);
    }
}

static void
load_bflogic_module(struct bflogic_module *m,
		    const char name[])
{
    char path[BF_MAXOBJECTNAME + PATH_MAX + 100];

    memset(m, 0, sizeof(struct bflogic_module));
    m->fork_mode = BF_FORK_DONT_FORK;

    find_module(path, name, ".bflogic");
    if ((m->handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL)) == NULL) {
	fprintf(stderr, "Failed to open module \"%s\" in \"%s\": %s.\n",
		name, path, dlerror());
	exit(BF_EXIT_OTHER);
    }
    m->preinit = load_module_function(m->handle, name, BF_FUN_BFLOGIC_PREINIT,
                                      true);
    m->command = load_module_function(m->handle, name, BF_FUN_BFLOGIC_COMMAND,
                                      false);
    m->init = load_module_function(m->handle, name, BF_FUN_BFLOGIC_INIT, true);
    m->message = load_module_function(m->handle, name, BF_FUN_BFLOGIC_MESSAGE,
                                      false);
    if (m->command != NULL && m->message == NULL) {
        fprintf(stderr, "Module \"%s\" in \"%s\" has command function but "
                "lacks message function.\n", name, path);
    }
}

static int
number_of_cpus(void)
{
    FILE *stream;
    char s[1000];
    int n_cpus = 0;

    /* This code is Linux specific... */
    
    if ((stream = fopen("/proc/cpuinfo", "rt")) == NULL) {
        fprintf(stderr, "Warning: could not determine number of processors, "
                "guessing 1.\n");
	return 1;
    }
    s[999] = '\0';
    while (fgets(s, 999, stream) != NULL) {
	if (strncasecmp(s, "processor", 9) == 0) {
	    n_cpus++;
	}
    }
    fclose(stream);
    if (n_cpus == 0) {
	n_cpus = 1;
    }
    return n_cpus;
}

static int
load_balance_filters(struct filter *pfilters[])
{
    uint32_t used_channels[BF_MAXCHANNELS / 32 + 1];
    int n, i, j, k, process;
    bool_t set;

    /* Step 1: make as many processes as possible, that is only follow the
       requirements: a) connected filters within same process, and b) mixed
       output within same process. */

    process = 0;
    for (n = 0; n < bfconf->n_filters; n++) {
        set = false;
        if (pfilters[n]->process != -1) {
            continue;
        }
        pfilters[n]->process = process;
        
        do {

            set = false;

            /* which filters are connected to the current? */
            for (i = 0; i < bfconf->n_filters; i++) {
                if (pfilters[i]->process != process) {
                    continue;
                }
                FOR_IN_AND_OUT {
                    for (j = 0; j < bfconf->filters[i].n_filters[IO]; j++) {
                        k = bfconf->filters[i].filters[IO][j];
                        if (pfilters[k]->process != process) {
                            pfilters[k]->process = process;
                            set = true;
                        }
                    }
                }
                
            }
        
            /* which filters mix to the same output? */
            
            memset(used_channels, 0, sizeof(used_channels));
            /* which outputs are in the current process? */
            for (i = 0; i < bfconf->n_filters; i++) {
                if (pfilters[i]->process != process) {
                    continue;
                }
                for (j = 0; j < bfconf->filters[i].n_channels[OUT]; j++) {
                    bit_set(used_channels,
                            bfconf->filters[i].channels[OUT][j]);
                }
            }
            /* move filters with same outputs to the current process */
            for (i = 0; i < bfconf->n_filters; i++) {
                if (pfilters[i]->process == process) {
                    continue;
                }
                for (j = 0; j < bfconf->filters[i].n_channels[OUT]; j++) {
                    if (bit_isset(used_channels,
                                  bfconf->filters[i].channels[OUT][j]))
                    {
                        pfilters[i]->process = process;
                        set = true;
                    }
                }
            }
            
        } while (set);
        
        process++;
    }

    /* Step 2: reduce the number of processes to the same as the number of CPUs.
       Since coefficients can be changed in runtime and so on, we just make a
       simple estimate, which may be dead wrong, but in those cases, the user
       have to configure manually. */

    j = 0;
    for (n = 0; n < process; n++) {
        for (i = 0; i < bfconf->n_filters; i++) {
            if (pfilters[i]->process == n) {
                pfilters[i]->process = j;
            }
        }
        if (++j == bfconf->n_cpus) {
            j = 0;
        }
    }

    return process < bfconf->n_cpus ? process - 1 : bfconf->n_cpus - 1;
}

void
bfconf_init(char filename[],
	    bool_t quiet,
            bool_t nodefault)
{
    struct iodev *iodevs[2][BF_MAXCHANNELS];
    struct filter *pfilters[BF_MAXFILTERS];
    struct coeff **coeffs = NULL;
    struct bffilter filters[BF_MAXFILTERS];
    struct dither_state *dither_state[BF_MAXCHANNELS];
    struct timeval tv1, tv2;
    int coeffs_capacity = 0;
    int largest_process = -1;
    int n_channels[2], min_prio, max_prio;
    int version_minor, version_major;
    uint32_t used_channels[2][BF_MAXCHANNELS / 32 + 1];
    uint32_t used_filters[BF_MAXFILTERS / 32 + 1];
    uint32_t local_used_channels[BF_MAXCHANNELS / 32 + 1];
    uint32_t apply_dither[BF_MAXCHANNELS / 32 + 1];
    uint32_t used_processes[BF_MAXPROCESSES / 32 + 1];
    uint32_t repeat_bitset = 0;
    int channels[2][BF_MAXCHANNELS];
    int n, i, j, k, io, token, virtch, physch, maxdelay[2];
    bool_t load_balance = false;
    uint64_t t1, t2;
    char str[200];

    gettimeofday(&tv1, NULL);
    timestamp(&t1);

    has_defaults = !nodefault;
    bfconf = emalloc(sizeof(struct bfconf));
    memset(bfconf, 0, sizeof(struct bfconf));
    FOR_IN_AND_OUT {
	bfconf->delay[IO] = emalloc(BF_MAXCHANNELS * sizeof(int));
	memset(bfconf->delay[IO], 0, BF_MAXCHANNELS * sizeof(int));
	bfconf->maxdelay[IO] = emalloc(BF_MAXCHANNELS * sizeof(int));
	memset(bfconf->maxdelay[IO], -1, BF_MAXCHANNELS * sizeof(int));
	bfconf->subdelay[IO] = emalloc(BF_MAXCHANNELS * sizeof(int));
	memset(bfconf->subdelay[IO], 0, BF_MAXCHANNELS * sizeof(int));
	bfconf->mute[IO] = emalloc(BF_MAXCHANNELS * sizeof(bool_t));
	memset(bfconf->mute[IO], 0, BF_MAXCHANNELS * sizeof(bool_t));
    }
    bfconf->sdf_length = -1;
    bfconf->quiet = quiet;
    bfconf->realsize = sizeof(float);
    bfconf->safety_limit = 0;

    if (!nodefault) {
        get_defaults();
    }

    if (filename != NULL && strcasecmp(filename, "stdin") == 0) {
	strncpy(current_filename, filename, PATH_MAX);
	current_filename[PATH_MAX-1] = '\0';
	yyin = stdin;
    } else {
	if (filename == NULL) {
	    strcpy(current_filename, default_config_file);
	} else {
	    strncpy(current_filename, filename, PATH_MAX);
	    current_filename[PATH_MAX-1] = '\0';
	}
	if ((yyin = fopen(current_filename, "rt")) == NULL) {
	    fprintf(stderr, "Could not open file \"%s\" for reading: %s.\n",
		    current_filename, strerror(errno));
	    exit(BF_EXIT_OTHER);
	}
    }
    lexlineno = 1;
    maxdelay[IN] = maxdelay[OUT] = 0;
    do {
	switch (token = yylex()) {
	case FIELD:
	    parse_setting(yylval.field, false, &repeat_bitset);
	    break;
	case COEFF:
            if (bfconf->n_coeffs == coeffs_capacity) {
                coeffs_capacity += 16;
                coeffs = erealloc(coeffs, coeffs_capacity * sizeof(void *));
            }
	    coeffs[bfconf->n_coeffs] = parse_coeff(false, bfconf->n_coeffs);
	    bfconf->n_coeffs++;
	    break;
	case INPUT:
	case OUTPUT:
	    io = (token == INPUT) ? IN : OUT;
	    if (bfconf->n_subdevs[io] == BF_MAXCHANNELS) {
		sprintf(str, "too many %s.\n",
			(io == IN) ? "inputs" : "outputs");
		parse_error(str);
	    }
	    iodevs[io][bfconf->n_subdevs[io]] =
		parse_iodev(false, io, bfconf->n_physical_channels[io],
			    bfconf->n_channels[io], &maxdelay[io]);
	    bfconf->n_physical_channels[io] +=
		iodevs[io][bfconf->n_subdevs[io]]->ch.used_channels;
	    bfconf->n_channels[io] +=
		iodevs[io][bfconf->n_subdevs[io]]->virtual_channels;
	    bfconf->n_subdevs[io]++;
	    break;
	case FILTER:
	    if (bfconf->n_filters == BF_MAXFILTERS) {
		parse_error("too many filters.\n");
	    }
	    pfilters[bfconf->n_filters] =
		parse_filter(false, bfconf->n_filters);
	    bfconf->n_filters++;
	    break;
	case EOF:
	    break;
	default:
	    parse_error("unexpected token.\n");
	    break;
	}
    } while (token != EOF);
    fclose(yyin);
    
    efree(default_coeff);
    efree(default_filter);

    if (!has_defaults) {
#ifdef CONVOLVER_NEEDS_CONFIGFILE
        field_mandatory_test(repeat_bitset, 0x8281, current_filename);
#else
        field_mandatory_test(repeat_bitset, 0x0281, current_filename);
#endif
    }

    pinfo("Internal resolution is %d bit floating point.\n",
	  bfconf->realsize * 8);

    /* do some sanity checks */
    FOR_IN_AND_OUT {
	if (bfconf->n_subdevs[IO] == 0) {
	    fprintf(stderr, "No %s defined.\n",
		    (IO == IN) ? "inputs" : "outputs");
	    exit(BF_EXIT_INVALID_CONFIG);
	}
    }
    if (bfconf->n_filters == 0) {
	fprintf(stderr, "No filters defined.\n");
	exit(BF_EXIT_INVALID_CONFIG);
    }
    if (bfconf->benchmark && bfconf->powersave) {
	fprintf(stderr, "The benchmark and powersave setting cannot "
                "both be set to true.\n");
	exit(BF_EXIT_INVALID_CONFIG);
    }

    /* create the channel arrays */
    FOR_IN_AND_OUT {
	bfconf->channels[IO] = emalloc(bfconf->n_channels[IO] *
				       sizeof(struct bfchannel));
	memset(bfconf->channels[IO], 0, bfconf->n_channels[IO] *
	       sizeof(struct bfchannel));
	bfconf->virt2phys[IO] = emalloc(bfconf->n_channels[IO] * sizeof(int));
	bfconf->n_virtperphys[IO] =
	    emalloc(bfconf->n_physical_channels[IO] * sizeof(int));
	memset(bfconf->n_virtperphys[IO], 0, bfconf->n_physical_channels[IO] *
	       sizeof(int));
	bfconf->phys2virt[IO] =
	    emalloc(bfconf->n_physical_channels[IO] * sizeof(int *));
	for (n = 0; n < bfconf->n_subdevs[IO]; n++) {
	    for (i = 0; i < iodevs[IO][n]->virtual_channels; i++) {
		virtch = iodevs[IO][n]->channel_intname[i];
		physch = iodevs[IO][n]->ch.channel_name[0] +
		    iodevs[IO][n]->virt2phys[i];
		bfconf->channels[IO][virtch].intname = virtch;
		bfconf->virt2phys[IO][virtch] = physch;
		bfconf->n_virtperphys[IO][physch]++;
		strcpy(bfconf->channels[IO][virtch].name,
		       iodevs[IO][n]->channel_name[i]);
		efree(iodevs[IO][n]->channel_name[i]);
	    }
	    for (i = 0; i < iodevs[IO][n]->ch.used_channels; i++) {
		physch = iodevs[IO][n]->ch.channel_name[i];
		bfconf->phys2virt[IO][physch] =
		    emalloc(bfconf->n_virtperphys[IO][physch] * sizeof(int));
		/* set all values to -1 */
		memset(bfconf->phys2virt[IO][physch], 0xFF,
		       bfconf->n_virtperphys[IO][physch] * sizeof(int));
	    }
	    for (i = 0; i < iodevs[IO][n]->virtual_channels; i++) {
		physch = iodevs[IO][n]->ch.channel_name[0] +
		    iodevs[IO][n]->virt2phys[i];
		for (k = 0; bfconf->phys2virt[IO][physch][k] != -1; k++);
		bfconf->phys2virt[IO][physch][k] =
		    iodevs[IO][n]->channel_intname[i];
	    }
	}
    }
    /* check for duplicate names */
    for (n = 0; n < bfconf->n_coeffs; n++) {
	for (i = 0; i < bfconf->n_coeffs; i++) {
	    if (n != i && strcmp(coeffs[i]->coeff.name,
				 coeffs[n]->coeff.name) == 0)
	    {
		fprintf(stderr, "Duplicate coefficient set names.\n");
		exit(BF_EXIT_INVALID_CONFIG);
	    }				 
	}
    }
    for (n = 0; n < bfconf->n_filters; n++) {
	for (i = 0; i < bfconf->n_filters; i++) {
	    if (n != i && strcmp(pfilters[i]->filter.name,
				 pfilters[n]->filter.name) == 0)
	    {
		fprintf(stderr, "Duplicate filter names.\n");
		exit(BF_EXIT_INVALID_CONFIG);
	    }				 
	}
    }
    FOR_IN_AND_OUT {
	for (n = 0; n < bfconf->n_channels[IO]; n++) {
	    for (i = 0; i < bfconf->n_channels[IO]; i++) {
		if (n != i && strcmp(bfconf->channels[IO][i].name,
				     bfconf->channels[IO][n].name) == 0)
		{
		    fprintf(stderr, "Duplicate channel names.\n");
		    exit(BF_EXIT_INVALID_CONFIG);
		}		 
	    }
	}
    }
    
    /* finish and sanity check filter structures */
    memset(used_channels, 0, sizeof(used_channels));
    memset(used_processes, 0, sizeof(used_processes));
    for (n = 0; n < bfconf->n_filters; n++) {
	/* coefficient name */
	if (pfilters[n]->coeff_name[0] == '\0') {
	    if (pfilters[n]->fctrl.coeff >= bfconf->n_coeffs) {
		fprintf(stderr, "Coeff index %d in filter %d/\"%s\" is out of "
			"range.\n", pfilters[n]->fctrl.coeff,
                        n, pfilters[n]->filter.name);
		exit(BF_EXIT_INVALID_CONFIG);
	    }
	} else {
	    pfilters[n]->fctrl.coeff = -1;
	    for (i = 0;
                 coeffs[i] != NULL && coeffs[i]->coeff.name != NULL;
                 i++)
            {
		if (strcmp(coeffs[i]->coeff.name,
			   pfilters[n]->coeff_name) == 0)
		{
		    pfilters[n]->fctrl.coeff = i;
		    break;
		}
	    }
	    if (pfilters[n]->fctrl.coeff == -1) {
		fprintf(stderr, "Coeff with name \"%s\" (in filter %d/\"%s\") "
                        "does not exist.\n", pfilters[n]->coeff_name,
                        n, pfilters[n]->filter.name);
		exit(BF_EXIT_INVALID_CONFIG);
	    }
	}

        if (pfilters[n]->process == -1) {
            if (n > 0 && !load_balance) {
                fprintf(stderr, "Cannot mix manual process settings with "
                        "automatic.\n");
		exit(BF_EXIT_INVALID_CONFIG);
            }
            load_balance = true;
        } else {
            if (pfilters[n]->process > largest_process) {
                largest_process = pfilters[n]->process;
            }
            bit_set(used_processes, pfilters[n]->process);
            if (n > 0 && load_balance) {
                fprintf(stderr, "Cannot mix manual process settings with "
                        "automatic.\n");
		exit(BF_EXIT_INVALID_CONFIG);
            }
            load_balance = false;
        }

	/* input/output names */
	FOR_IN_AND_OUT {
	    
	    /* channel input/outputs */
	    memset(local_used_channels, 0, sizeof(local_used_channels));
	    for (j = 0; j < pfilters[n]->filter.n_channels[IO]; j++) {
		if (pfilters[n]->channel_name[IO][j] == NULL) {
		    if (pfilters[n]->filter.channels[IO][j] < 0 ||
			pfilters[n]->filter.channels[IO][j] >=
			bfconf->n_channels[IO])
		    {
			fprintf(stderr, "%s channel index %d in filter "
                                "%d/\"%s\" is out of range.\n",
				(IO == IN) ? "Input" : "Output",
				pfilters[n]->filter.channels[IO][j],
                                n, pfilters[n]->filter.name);
			exit(BF_EXIT_INVALID_CONFIG);
		    }
		} else {
		    pfilters[n]->filter.channels[IO][j] = -1;
                    for (i = 0; i < bfconf->n_channels[IO]; i++) {
			if (strcmp(bfconf->channels[IO][i].name,
				   pfilters[n]->channel_name[IO][j]) == 0)
			{
			    pfilters[n]->filter.channels[IO][j] = i;
			    break;
			}
		    }
		    if (pfilters[n]->filter.channels[IO][j] == -1) {
			fprintf(stderr, "%s channel with name \"%s\" (in "
				"filter %d/\"%s\") does not exist.\n",
				(IO == IN) ? "Input" : "Output",
				pfilters[n]->channel_name[IO][j],
                                n, pfilters[n]->filter.name);
			exit(BF_EXIT_INVALID_CONFIG);
		    }
		    efree(pfilters[n]->channel_name[IO][j]);
		}
		bit_set(used_channels[IO], pfilters[n]->filter.channels[IO][j]);
		if (bit_isset(local_used_channels,
			      pfilters[n]->filter.channels[IO][j]))
		{
		    fprintf(stderr, "Duplicate channels in filter %d/\"%s\".\n",
                            n, pfilters[n]->filter.name);
		    exit(BF_EXIT_INVALID_CONFIG);		    
		}
		bit_set(local_used_channels,
			pfilters[n]->filter.channels[IO][j]);
	    }

	    /* filter input/outputs */
	    memset(used_filters, 0, sizeof(used_filters));
	    for (j = 0; j < pfilters[n]->filter.n_filters[IO]; j++) {
		if (pfilters[n]->filter_name[IO][j] == NULL) {
		    if (pfilters[n]->filter.filters[IO][j] < 0 ||
			pfilters[n]->filter.filters[IO][j] >=
			bfconf->n_filters)
		    {
			fprintf(stderr, "%s filter index %d in filter "
                                "%d/\"%s\" is out of range.\n",
				(IO == IN) ? "Input" : "Output",
				pfilters[n]->filter.filters[IO][j],
                                n, pfilters[n]->filter.name);
			exit(BF_EXIT_INVALID_CONFIG);
		    }
		} else {
		    pfilters[n]->filter.filters[IO][j] = -1;
		    for (i = 0; i < bfconf->n_filters; i++) {
			if (strcmp(pfilters[i]->filter.name,
				   pfilters[n]->filter_name[IO][j]) == 0)
			{
			    pfilters[n]->filter.filters[IO][j] = i;
			    break;
			}
		    }
		    if (pfilters[n]->filter.filters[IO][j] == -1) {
			fprintf(stderr, "%s filter with name \"%s\" (in "
				"filter %d/\"%s\") does not exist.\n",
				(IO == IN) ? "Input" : "Output",
				pfilters[n]->filter_name[IO][j],
                                n, pfilters[n]->filter.name);
			exit(BF_EXIT_INVALID_CONFIG);
		    }
		    efree(pfilters[n]->filter_name[IO][j]);
		}
		if (bit_isset(used_filters,
			      pfilters[n]->filter.filters[IO][j]))
		{
		    fprintf(stderr, "Duplicate filters in filter %d/\"%s\".\n",
                            n, pfilters[n]->filter.name);
		    exit(BF_EXIT_INVALID_CONFIG);
		}
		bit_set(used_filters, pfilters[n]->filter.filters[IO][j]);
	    }	    
	}

	/* finish delayblocks number */
	if (pfilters[n]->fctrl.delayblocks > bfconf->n_blocks - 1) {
	    fprintf(stderr, "Delay in filter %d/\"%s\" is too large (max "
                    "allowed is %d blocks, max blocks - 1).\n",
                    n, pfilters[n]->filter.name, bfconf->n_blocks - 1);
	    exit(BF_EXIT_INVALID_CONFIG);
	}
    }

    /* check if all in/out channels are used in the filters */
    FOR_IN_AND_OUT {
	for (n = 0; n < bfconf->n_channels[IO]; n++) {
	    if (!bit_isset(used_channels[IO], n)) {
		pinfo("Warning: %s channel \"%s\" is not used.\n",
		      (IO == IN) ? "input" : "output",
		      bfconf->channels[IO][n].name);
	    }
	}
    }

    /* check if all processes are used (if manually set) */
    for (n = 0; n <= largest_process; n++) {
	if (!bit_isset(used_processes, n)) {
	    fprintf(stderr, "The range of process indexes among filters is "
		    "broken.\n");
	    exit(BF_EXIT_INVALID_CONFIG);
	}
    }

    /* fill in the filters and initfctrl array */
    bfconf->filters = emalloc(bfconf->n_filters * sizeof(struct bffilter));
    bfconf->initfctrl = emalloc(bfconf->n_filters *
				sizeof(struct bffilter_control));
    for (n = 0; n < bfconf->n_filters; n++) {
	bfconf->filters[n] = pfilters[n]->filter;
	bfconf->initfctrl[n] = pfilters[n]->fctrl;
    }

    /* check so filters are connected, that is that output to a filter shows
       as input from source filter at the destination filter */
    for (n = 0; n < bfconf->n_filters; n++) {
	for (i = 0; i < bfconf->filters[n].n_filters[OUT]; i++) {
	    k = bfconf->filters[n].filters[OUT][i];
	    for (j = 0; j < bfconf->filters[k].n_filters[IN]; j++) {
		if (bfconf->filters[k].filters[IN][j] == n) {
		    break;
		}
	    }
	    if (j == bfconf->filters[k].n_filters[IN]) {
		fprintf(stderr,
"Output to filter %d/\"%s\" from filter %d/\"%s\" must exist\n\
  as input at at the destination filter %d/\"%s\".\n",
                        k, bfconf->filters[k].name, n, bfconf->filters[n].name,
                        k, bfconf->filters[k].name);
		exit(BF_EXIT_INVALID_CONFIG);
	    }
	}
	for (i = 0; i < bfconf->filters[n].n_filters[IN]; i++) {
	    k = bfconf->filters[n].filters[IN][i];
	    for (j = 0; j < bfconf->filters[k].n_filters[OUT]; j++) {
		if (bfconf->filters[k].filters[OUT][j] == n) {
		    break;
		}
	    }
	    if (j == bfconf->filters[k].n_filters[OUT]) {
		fprintf(stderr,
"Input from filter %d/\"%s\" in filter %d/\"%s\" must exist\n\
  as output in the source filter %d/\"%s\".\n",
                        k, bfconf->filters[k].name, n, bfconf->filters[n].name,
                        k, bfconf->filters[k].name);
		exit(BF_EXIT_INVALID_CONFIG);
	    }
	}
    }
    
    /* check so there are no filter loops */
    for (n = 0; n < bfconf->n_filters; n++) {
	if (filter_loop(n, n)) {
	    fprintf(stderr, "Filter %d is involved in a loop.\n", n);
	    exit(BF_EXIT_INVALID_CONFIG);
	}
    }

    /* estimate a load balancing for filters (if not manually set) */
    bfconf->n_cpus = number_of_cpus();
    if (load_balance) {
        largest_process = load_balance_filters(pfilters);
    }

/*    if (convolver_init != NULL) {*/
	/* initialise convolver */
	if (!convolver_init(convolver_config, bfconf->filter_length,
                            bfconf->realsize))
        {
	    fprintf(stderr, "Convolver initialisation failed.\n");
	    exit(BF_EXIT_OTHER);
	}
	efree(convolver_config);
/*    }*/

    /* init subdelay */
    if (bfconf->sdf_length < 0) {
        bfconf->use_subdelay[IN] = false;
        bfconf->use_subdelay[OUT] = false;
    } else if (2 * bfconf->sdf_length + 1 > bfconf->filter_length) {
        /* note: this check depends on the internals of
           delay_subsample_init() */
        fprintf(stderr, "The filter_length must be larger than "
                "2 x sdf_length + 1.\n"); 
	exit(BF_EXIT_INVALID_CONFIG);
    }
    if (bfconf->use_subdelay[IN] || bfconf->use_subdelay[OUT]) {
        if (!delay_subsample_init(BF_SAMPLE_SLOTS,
                                  bfconf->sdf_length,
                                  bfconf->sdf_beta,
                                  bfconf->filter_length,
                                  bfconf->realsize))
        {
            bf_exit(BF_EXIT_OTHER);
            return;
        }
    }

    /* load coefficients */
    bfconf->coeffs_data = emalloc(bfconf->n_coeffs * sizeof(void **));
    bfconf->coeffs = emalloc(bfconf->n_coeffs * sizeof(struct bfcoeff));
    if (bfconf->n_coeffs == 1) {
        pinfo("Loading coefficient set...");
    } else if (bfconf->n_coeffs > 1) {
        pinfo("Loading %d coefficient sets...", bfconf->n_coeffs);
    }
    for (n = 0; n < bfconf->n_coeffs; n++) {
	if (coeffs[n]->coeff.n_blocks <= 0) {
	    coeffs[n]->coeff.n_blocks = bfconf->n_blocks;
	} else if (coeffs[n]->coeff.n_blocks > bfconf->n_blocks) {
	    fprintf(stderr, "Too many blocks in coeff %d.\n", n);
	    exit(BF_EXIT_INVALID_CONFIG);
	}
	bfconf->coeffs_data[n] = load_coeff(coeffs[n], n, bfconf->realsize);
	bfconf->coeffs[n] = coeffs[n]->coeff;
	efree(coeffs[n]);
    }
    if (bfconf->n_coeffs > 0) {
        pinfo("finished.\n");
    }
    efree(coeffs);

    /* shorten mute array */
    FOR_IN_AND_OUT {
	bfconf->mute[IO] = erealloc(bfconf->mute[IO], bfconf->n_channels[IO] *
				    sizeof(bool_t));
	bfconf->delay[IO] = erealloc(bfconf->delay[IO],
				     bfconf->n_channels[IO] * sizeof(int));
    }
    
    /* derive filter_process from filters */
    bfconf->n_processes = largest_process + 1;
    bfconf->fproc = emalloc(bfconf->n_processes *
			    sizeof(struct filter_process));
    memset(bfconf->fproc, 0, bfconf->n_processes *
	   sizeof(struct filter_process));
    for (n = k = 0; n < bfconf->n_processes; n++) {
	n_channels[IN] = n_channels[OUT] = 0;
	memset(used_channels, 0, sizeof(used_channels));
	for (i = 0; i < bfconf->n_filters; i++) {
	    if (pfilters[i]->process == n) {
		filters[bfconf->fproc[n].n_filters] = pfilters[i]->filter;
		bfconf->fproc[n].n_filters++;
		FOR_IN_AND_OUT {
		    for (j = 0; j < pfilters[i]->filter.n_channels[IO]; j++) {
			if (!bit_isset(used_channels[IO],
				       pfilters[i]->filter.channels[IO][j]))
			{
			    channels[IO][n_channels[IO]] =
				pfilters[i]->filter.channels[IO][j];
			    n_channels[IO]++;
			}
			bit_set(used_channels[IO],
				pfilters[i]->filter.channels[IO][j]);
		    }
		}
	    }
	}

	FOR_IN_AND_OUT {
	    bfconf->fproc[n].n_unique_channels[IO] = n_channels[IO];
	    bfconf->fproc[n].unique_channels[IO] =
		emalloc(n_channels[IO] * sizeof(int));
	    memcpy(bfconf->fproc[n].unique_channels[IO], channels[IO],
		   n_channels[IO] * sizeof(int));
	}
	
	bfconf->fproc[n].filters =
	    emalloc(bfconf->fproc[n].n_filters * sizeof(struct bffilter));
	memcpy(bfconf->fproc[n].filters, filters,
	       bfconf->fproc[n].n_filters * sizeof(struct bffilter));
    }

    /* check so there are no output mixing between processes */
    for (n = 0; n < bfconf->n_processes; n++) {
	memset(used_channels, 0, sizeof(used_channels));
	memset(used_filters, 0, sizeof(used_filters));
	for (i = 0; i < bfconf->fproc[n].n_filters; i++) {
	    for (j = 0; j < bfconf->fproc[n].filters[i].n_channels[OUT]; j++) {
		bit_set(used_channels[OUT],
			bfconf->fproc[n].filters[i].channels[OUT][j]);
	    }
	    for (j = 0; j < bfconf->fproc[n].filters[i].n_filters[OUT]; j++) {
		bit_set(used_filters,
			bfconf->fproc[n].filters[i].filters[OUT][j]);
	    }
	}
	for (k = 0; k < bfconf->n_processes; k++) {
	    if (k != n) {
		for (i = 0; i < bfconf->fproc[k].n_filters; i++) {
		    if (bit_isset(used_filters,
				  bfconf->fproc[k].filters[i].intname))
		    {
			fprintf(stderr, "Connected filters must be processed "
				"within the same process.\n");
			exit(BF_EXIT_INVALID_CONFIG);
		    }
		    for (j = 0;
			 j < bfconf->fproc[k].filters[i].n_channels[OUT]; j++)
		    {
			if (bit_isset(used_channels[OUT], bfconf->
				      fproc[k].filters[i].channels[OUT][j]))
			{
			    fprintf(stderr, "Mixed outputs must be processed "
				    "within the same process.\n");
			    exit(BF_EXIT_INVALID_CONFIG);
			}
		    }
		}
	    }
	}
    }
    
    /* sort filters in processes in the correct process order */
    for (n = 0; n < bfconf->n_processes; n++) {
	memset(used_filters, 0, sizeof(used_filters));
	j = 0;
	while (j < bfconf->fproc[n].n_filters) {
	    for (i = 0; i < bfconf->fproc[n].n_filters; i++) {
		if (bit_isset(used_filters,
			      bfconf->fproc[n].filters[i].intname))
		{
		    continue;
		}
		for (k = 0;
		     k < bfconf->fproc[n].filters[i].n_filters[IN];
		     k++)
		{
		    if (!bit_isset(used_filters,
				   bfconf->fproc[n].filters[i].filters[IN][k]))
		    {
			k = -1;
			break;
		    }
		}
		if (k != -1) {
		    filters[j] = bfconf->fproc[n].filters[i];
		    bit_set(used_filters, filters[j].intname);
		    j++;
		}
	    }
	}
	memcpy(bfconf->fproc[n].filters, filters,
	       bfconf->fproc[n].n_filters * sizeof(struct bffilter));
    }
    
    /* load bflogic modules */
    if (bfconf->n_logicmods > 0) {
        bfconf->logicmods = emalloc(bfconf->n_logicmods *
                                    sizeof(struct bflogic_module));
        memset(bfconf->logicmods, 0, bfconf->n_logicmods *
               sizeof(struct bflogic_module));
        bfconf->logicnames = emalloc(bfconf->n_logicmods * sizeof(char *));
    }
    for (n = i = 0; n < bfconf->n_logicmods; n++) {
	load_bflogic_module(&bfconf->logicmods[n], logic_names[n]);
        config_params = logic_params[n];
        config_params_pos = 0;
        version_major = BF_VERSION_MAJOR;
        version_minor = BF_VERSION_MINOR;        
	if (bfconf->logicmods[n].preinit(&version_major,
                                         &version_minor,
                                         get_config_token,
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
                                         &bfconf->logicmods[n].bfevents,
                                         &bfconf->logicmods[n].fork_mode,
                                         (int)bfconf->debug) == -1)
	{
	    fprintf(stderr, "Error at line %d for logic module \"%s\".\n",
                    lexlineno, logic_names[n]);
	    exit(BF_EXIT_INVALID_CONFIG);
	}
        if (version_major != BF_VERSION_MAJOR) {
	    fprintf(stderr, "Logic module \"%s\" reported an incompatible "
                    "major version (got %d, expected %d).\n",
                    logic_names[n], version_major, BF_VERSION_MAJOR);
	    exit(BF_EXIT_OTHER);
        }
        free_params(logic_params[n]);
	if (bfconf->logicmods[n].fork_mode != BF_FORK_DONT_FORK) {
	    i++;
	}
	if (bfconf->logicmods[n].bfevents.fdevents != 0) {
	    if (pipe(bfconf->logicmods[n].event_pipe) == -1) {
		fprintf(stderr, "Failed to create pipe: %s.\n",
			strerror(errno));
		exit(BF_EXIT_OTHER);
	    }
	} else {
	    bfconf->logicmods[n].event_pipe[0] = -1;
	    bfconf->logicmods[n].event_pipe[1] = -1;
	}
	bfconf->logicnames[n] = logic_names[n];
    }
    if (bfconf->n_processes + i >= BF_MAXPROCESSES) {
	fprintf(stderr, "Too many processes.\n");
	exit(BF_EXIT_INVALID_CONFIG);
    }
    
    /* load bfio modules */
    bfconf->iomods = emalloc(BF_MAXMODULES * sizeof(struct bfio_module));
    memset(bfconf->iomods, 0, BF_MAXMODULES * sizeof(struct bfio_module));
    bfconf->ionames = emalloc(BF_MAXMODULES * sizeof(char *));
    FOR_IN_AND_OUT {
	for (n = 0; n < bfconf->n_subdevs[IO]; n++) {
	    for (i = 0; i < bfconf->n_iomods; i++) {
		if (strcmp(bfconf->ionames[i],
			   iodevs[IO][n]->device_name) == 0)
		{
		    break;
		}
	    }
	    if (i != bfconf->n_iomods) {
		continue;
	    }
	    load_bfio_module(&bfconf->iomods[bfconf->n_iomods],
			     iodevs[IO][n]->device_name);
            if (bfconf->iomods[bfconf->n_iomods].iscallback) {
                bfconf->callback_io = true;
            } else {
                bfconf->blocking_io = true;
            }
	    bfconf->ionames[bfconf->n_iomods] =
		estrdup(iodevs[IO][n]->device_name);
	    if (++bfconf->n_iomods == BF_MAXMODULES) {
		fprintf(stderr, "Too many modules.\n");
		exit(BF_EXIT_INVALID_CONFIG);
	    }
	}
    }
    bfconf->iomods = erealloc(bfconf->iomods, bfconf->n_iomods *
			      sizeof(struct bfio_module));
    bfconf->ionames = erealloc(bfconf->ionames, bfconf->n_iomods *
			       sizeof(char *));
    /* derive dai_subdevices from inputs and outputs */
    FOR_IN_AND_OUT {
	bfconf->subdevs[IO] = emalloc(bfconf->n_subdevs[IO] *
				      sizeof(struct dai_subdevice));
	memset(bfconf->subdevs[IO], 0, bfconf->n_subdevs[IO] *
	       sizeof(struct dai_subdevice));
	for (n = 0; n < bfconf->n_subdevs[IO]; n++) {
	    for (i = 0; i < bfconf->n_iomods; i++) {
		if (strcmp(bfconf->ionames[i],
			   iodevs[IO][n]->device_name) == 0)
		{
		    break;
		}
	    }
	    bfconf->subdevs[IO][n].module = i;
            config_params = iodevs[IO][n]->device_params;
            config_params_pos = 0;
            bfconf->subdevs[IO][n].sched_policy = -1;
            if (iodevs[IO][n]->auto_format) {
                iodevs[IO][n]->ch.sf.format = BF_SAMPLE_FORMAT_AUTO;
            }
            version_major = BF_VERSION_MAJOR;
            version_minor = BF_VERSION_MINOR;        
	    bfconf->subdevs[IO][n].params = 
		bfconf->iomods[i].preinit(&version_major,
                                          &version_minor,
                                          get_config_token,
                                          IO,
                                          &iodevs[IO][n]->ch.sf.format,
                                          bfconf->sampling_rate,
                                          iodevs[IO][n]->ch.open_channels,
                                          &bfconf->subdevs[IO][n].uses_clock,
                                          &bfconf->subdevs[IO][n].sched_policy,
                                          &bfconf->subdevs[IO][n].sched_param,
                                          (int)bfconf->debug);
            if (default_iodev[IO] == NULL ||
                config_params != default_iodev[IO]->device_params)
            {
                free_params(config_params);
            }
            if (version_major != BF_VERSION_MAJOR) {
                fprintf(stderr, "Module \"%s\" reported an incompatible "
                        "major version (got %d, expected %d).\n",
                        bfconf->ionames[i], version_major, BF_VERSION_MAJOR);
                exit(BF_EXIT_OTHER);
            }
	    if (bfconf->subdevs[IO][n].params == NULL) {
		fprintf(stderr, "Error at line %d for %s device using "
                        "module \"%s\".\n", lexlineno,
                        (IO == IN) ? "input" : "output",
			bfconf->ionames[i]);
		exit(BF_EXIT_INVALID_CONFIG);
	    }
	    if (IO == IN) {
		if (!bfconf->iomods[i].iscallback &&
                    bfconf->iomods[i].read == NULL)
                {
		    fprintf(stderr, "Module \"%s\" does not support input.\n",
			    bfconf->ionames[i]);
		    exit(BF_EXIT_INVALID_CONFIG);
		}
	    } else {
		if (!bfconf->iomods[i].iscallback &&
                    bfconf->iomods[i].write == NULL)
                {
		    fprintf(stderr, "Module \"%s\" does not support output.\n",
			    bfconf->ionames[i]);
		    exit(BF_EXIT_INVALID_CONFIG);
		}
	    }
	    bfconf->subdevs[IO][n].uses_clock =
		!!bfconf->subdevs[IO][n].uses_clock;
	    if (bfconf->subdevs[IO][n].uses_clock) {
		bfconf->realtime_priority = true;
	    }
            if ((bfconf->subdevs[IO][n].uses_clock ||
                 bfconf->iomods[i].iscallback) &&
                 bfconf->iomods[i].synch_start == NULL)
            {
                fprintf(stderr, "Module \"%s\" lacks the synch_start/stop "
                        "functions.\n", bfconf->ionames[i]);
                exit(BF_EXIT_INVALID_CONFIG);
            }                
            if (!bfconf->subdevs[IO][n].uses_clock &&
                bfconf->iomods[i].start == NULL &&
                !bfconf->iomods[i].iscallback)
            {
                fprintf(stderr, "Module \"%s\" lacks the start/stop "
                        "functions.\n", bfconf->ionames[i]);
                exit(BF_EXIT_INVALID_CONFIG);
            }
            if (IO == OUT && bfconf->subdevs[IO][n].uses_clock &&
                !bfconf->iomods[i].iscallback)
            {
                bfconf->synched_write = true;
            }
            if (iodevs[IO][n]->auto_format) {
                if (bf_sampleformat_size(iodevs[IO][n]->ch.sf.format) <= 0) {
                    fprintf(stderr, "Module \"%s\" could not decide sample "
                            "format.\n", bfconf->ionames[i]);
                    exit(BF_EXIT_OTHER);
                }
                parse_sample_format(&iodevs[IO][n]->ch.sf,
                                    bf_strsampleformat(iodevs[IO][n]->
                                                       ch.sf.format), false);
            }
            bfconf->subdevs[IO][n].channels = iodevs[IO][n]->ch;
	}
    }

    /* check which output channels that have dither applied */
    memset(apply_dither, 0, sizeof(apply_dither));
    for (n = j = 0; n < bfconf->n_subdevs[OUT]; n++) {
	if (!iodevs[OUT][n]->apply_dither) {
	    continue;
	}
	/* verify if it is feasible to apply dither */
	if (iodevs[OUT][n]->ch.sf.isfloat) {
            if (!iodevs[OUT][n]->auto_format) {
                pinfo("Warning: cannot dither floating point format "
                      "(outputs %d - %d).\n",
                      iodevs[OUT][n]->channel_intname[0],
                      iodevs[OUT][n]->channel_intname[iodevs[OUT][n]->
                                                     virtual_channels - 1]);
            }
	    continue;
	}
	if (bfconf->realsize == sizeof(float) &&
	    iodevs[OUT][n]->ch.sf.sbytes > 2)
	{
            if (!iodevs[OUT][n]->auto_format) {
                pinfo("Warning: internal resolution not high enough to "
                      "dither (outputs %d - %d).\n",
                      iodevs[OUT][n]->channel_intname[0],
                      iodevs[OUT][n]->channel_intname[iodevs[OUT][n]->
                                                     virtual_channels - 1]);
            }
	    continue;
	}
	if (iodevs[OUT][n]->ch.sf.sbytes >= 4) {
            if (!iodevs[OUT][n]->auto_format) {
                pinfo("Warning: cannot apply dither to 32 bit format "
                      "(outputs %d - %d).\n",
                      iodevs[OUT][n]->channel_intname[0],
                      iodevs[OUT][n]->channel_intname[iodevs[OUT][n]->
                                                     virtual_channels - 1]);
            }
	    continue;
	}
	/* which physical channels to apply dither on */
	for (i = 0; i < iodevs[OUT][n]->ch.used_channels; i++) {
	    bit_set(apply_dither, iodevs[OUT][n]->ch.channel_name[i]);
	    j++;
	}
    }
    
    /* initialise dither array */
    bfconf->dither_state = emalloc(bfconf->n_physical_channels[OUT] *
				   sizeof(struct dither_state *));
    memset(bfconf->dither_state, 0, bfconf->n_physical_channels[OUT] *
	   sizeof(struct dither_state *));
    if (j > 0) {
	if (!dither_init(j, bfconf->sampling_rate, bfconf->realsize,
			 bfconf->max_dither_table_size, bfconf->filter_length,
			 dither_state))
	{
	    exit(BF_EXIT_OTHER);
	}
	for (n = j = 0; n < bfconf->n_physical_channels[OUT]; n++) {
	    if (bit_isset(apply_dither, n)) {
		bfconf->dither_state[n] = dither_state[j];
		j++;
	    } else {
		bfconf->dither_state[n] = NULL;
	    }
	}
    }

    /* calculate which sched priorities to use */
    bfconf->realtime_maxprio = 4;
    bfconf->realtime_midprio = 3;
    bfconf->realtime_minprio = 2;
    bfconf->realtime_usermaxprio = 1;
    min_prio = -1;
    max_prio = 0;
    FOR_IN_AND_OUT {
	for (n = 0; n < bfconf->n_subdevs[IO]; n++) {
            if (bfconf->subdevs[IO][n].sched_policy != -1) {
                if (bfconf->realtime_priority &&
                    bfconf->subdevs[IO][n].uses_clock &&
                    bfconf->subdevs[IO][n].sched_policy != SCHED_FIFO &&
                    bfconf->subdevs[IO][n].sched_policy != SCHED_RR)
                {
                    fprintf(stderr, "\
External scheduling belonging to module \"%s\" must be realtime.\n",
                            bfconf->ionames[bfconf->subdevs[IO][n].module]);
                    exit(BF_EXIT_INVALID_CONFIG);
                }
                if (bfconf->realtime_priority) {
                    if (max_prio <
                        bfconf->subdevs[IO][n].sched_param.sched_priority)
                    {
                        max_prio = bfconf->subdevs[IO][n].
                            sched_param.sched_priority;
                    }
                    if (bfconf->subdevs[IO][n].sched_param.sched_priority <
                        min_prio || min_prio == -1)
                    {
                        min_prio = bfconf->subdevs[IO][n].
                            sched_param.sched_priority;
                    }
                    if (bfconf->realtime_midprio >
                        bfconf->subdevs[IO][n].sched_param.sched_priority)
                    {
                        fprintf(stderr, "\
External scheduling priority for module \"%s\" is lower than %d.\n",
                                bfconf->ionames[bfconf->subdevs[IO][n].module],
                                bfconf->realtime_midprio);
                        exit(BF_EXIT_INVALID_CONFIG);
                    }
                }
            }
        }
    }
    if (min_prio != -1) {
        bfconf->realtime_midprio = max_prio;
        bfconf->realtime_minprio = min_prio - 1;
        bfconf->realtime_usermaxprio = min_prio - 2;
    }
    if (bfconf->realtime_midprio > bfconf->realtime_maxprio) {
        bfconf->realtime_maxprio = bfconf->realtime_midprio + 1;
    }
    if (bfconf->realtime_maxprio > sched_get_priority_max(SCHED_FIFO)) {
        fprintf(stderr, "Max priority exceeds maximum possible (%d > %d).\n",
                bfconf->realtime_maxprio, sched_get_priority_max(SCHED_FIFO));
        exit(BF_EXIT_INVALID_CONFIG);
    }
    if (bfconf->realtime_priority) {
        pinfo("Realtime priorities are min = %d, usermax = %d, "
              "mid = %d and max = %d.\n",
              bfconf->realtime_minprio, bfconf->realtime_usermaxprio,
              bfconf->realtime_midprio, bfconf->realtime_maxprio);
    }
        
    /* calculate maximum possible flowthrough blocks */
    bfconf->flowthrough_blocks = bfconf->n_blocks +
        (maxdelay[IN] + bfconf->filter_length - 1) / bfconf->filter_length +
        (maxdelay[OUT] + bfconf->filter_length - 1) / bfconf->filter_length;

    FOR_IN_AND_OUT {
        if (default_iodev[IO] == NULL) {
            continue;
        }
	free_params(default_iodev[IO]->device_params);
	efree(default_iodev[IO]->ch.channel_selection);
	efree(default_iodev[IO]->ch.channel_name);
	efree(default_iodev[IO]);
    }

    
    /* estimate CPU clock rate */
    gettimeofday(&tv2, NULL);
    timestamp(&t2);
    timersub(&tv2, &tv1, &tv2);
    if (tv2.tv_sec == 0 && tv2.tv_usec < 100000) {
        usleep(100000 - tv2.tv_usec);
        gettimeofday(&tv2, NULL);
        timestamp(&t2);
        timersub(&tv2, &tv1, &tv2);
    }
    bfconf->cpu_mhz = (double)
        ((long double)(t2 - t1) /
         (long double)(tv2.tv_sec * 1000000 + tv2.tv_usec));
#ifdef TIMESTAMP_NOT_CLOCK_CYCLE_COUNTER
    pinfo("Warning: no support for clock cycle counter on this platform.\n"
          "  Timers for benchmarking may be unreliable.\n");
#else    
    pinfo("Estimated CPU clock rate is %.3f MHz. CPU count is %d.\n",
          bfconf->cpu_mhz, bfconf->n_cpus);
#endif    

    if (load_balance && bfconf->n_cpus > 1) {
        for (n = 0; n <= largest_process; n++) {
            pinfo("Filters in process %d: ", n);
            for (i = 0; i < bfconf->n_filters; i++) {
                if (pfilters[i]->process == n) {
                    pinfo("%d ", i);
                }
            }
            pinfo("\n");
        }
    }

    /* free allocated memory */
    FOR_IN_AND_OUT {
	for (n = 0; n < bfconf->n_subdevs[IO]; n++) {
	    efree(iodevs[IO][n]);
        }
    }
}
