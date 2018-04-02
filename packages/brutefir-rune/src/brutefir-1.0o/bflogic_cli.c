/*
 * (c) Copyright 2001 - 2006, 2009, 2013 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <math.h>
#include <fcntl.h>
#include <termios.h>

#define IS_BFLOGIC_MODULE
#include "timermacros.h"
#include "bfmod.h"
#include "defs.h"
#include "log2.h"
#include "bit.h"
#include "inout.h"
#include "fdrw.h"

#define WELCOME_TEXT "\nWelcome to BruteFIR, type \"help\" for help.\n\n"
#define PROMPT_TEXT "> "

#define HELP_TEXT "\n\
Commands:\n\
\n\
lf -- list filters.\n\
lc -- list coeffient sets.\n\
li -- list inputs.\n\
lo -- list outputs.\n\
lm -- list modules.\n\
\n\
cfoa -- change filter output attenuation.\n\
        cfoa <filter> <output> <attenuation|Mmultiplier>\n\
cfia -- change filter input attenuation.\n\
        cfia <filter> <input> <attenuation|Mmultiplier>\n\
cffa -- change filter filter-input attenuation.\n\
        cffa <filter> <filter-input> <attenuation|Mmultiplier>\n\
cfc  -- change filter coefficients.\n\
        cfc <filter> <coeff>\n\
cfd  -- change filter delay. (may truncate coeffs!)\n\
        cfd <filter> <delay blocks>\n\
cod  -- change output delay.\n\
        cod <output> <delay> [<subdelay>]\n\
cid  -- change input delay.\n\
        cid <input> <delay> [<subdelay>]\n\
tmo  -- toggle mute output.\n\
        tmo <output>\n\
tmi  -- toggle mute input.\n\
        tmi <input>\n\
imc  -- issue input module command.\n\
        imc <index> <command>\n\
omc  -- issue output module command.\n\
        omc <index> <command>\n\
lmc  -- issue logic module command.\n\
        lmc <module> <command>\n\
\n\
sleep -- sleep for the given number of seconds [and ms], or blocks.\n\
         sleep 10 (sleep 10 seconds).\n\
         sleep b10 (sleep 10 blocks).\n\
         sleep 0 300 (sleep 300 milliseconds).\n\
abort -- terminate immediately.\n\
tp    -- toggle prompt.\n\
ppk   -- print peak info, channels/samples/max dB.\n\
rpk   -- reset peak meters.\n\
upk   -- toggle print peak info on changes.\n\
rti   -- print current realtime index.\n\
quit  -- close connection.\n\
help  -- print this text.\n\
\n\
Notes:\n\
\n\
- When entering several commands on a single line,\n\
  separate them with semicolons (;).\n\
- Inputs/outputs/filters can be given as index\n\
  numbers or as strings between quotes (\"\").\n\
\n\
"

#define MAXCMDLINE 4096

#define FILTER_ID 1
#define INPUT_ID  2
#define OUTPUT_ID 3
#define COEFF_ID  4

static volatile struct bffilter_control *fctrl;
static int n_filters;
static const struct bffilter *filters;
static int n_coeffs;
static const struct bfcoeff *coeffs;
static const int *n_channels;
static const struct bfchannel **channels;
static int line_speed;
static int port, port2;
static char *lport = NULL;
static char *script = NULL;
static struct bfaccess *bfaccess;
static int n_maxblocks;
static int block_length;

static bool_t print_peak_updates = false;
static bool_t print_prompt = true;
static bool_t print_commands = false;
static bool_t debug = false;
static char error[1024];

struct sleep_task {
    bool_t do_sleep;
    bool_t block_sleep;
    unsigned int blocks;
    unsigned int seconds;
    unsigned int useconds;
};

struct state {
    struct bffilter_control fctrl[BF_MAXFILTERS];
    bool_t fchanged[BF_MAXFILTERS];
    int delay[2][BF_MAXCHANNELS];
    int subdelay[2][BF_MAXCHANNELS];
    bool_t toggle_mute[2][BF_MAXCHANNELS];
};

static struct state newstate;

static void
clear_changes(void)
{
    int n, i;

    if (fctrl == NULL) {
        return;
    }
    for (n = 0; n < n_filters; n++) {
        newstate.fctrl[n].coeff = fctrl[n].coeff;
        newstate.fctrl[n].delayblocks = fctrl[n].delayblocks;
        for (i = 0; i < filters[n].n_channels[OUT]; i++) {
            newstate.fctrl[n].scale[OUT][i] = fctrl[n].scale[OUT][i];
        }
        for (i = 0; i < filters[n].n_channels[IN]; i++) {
            newstate.fctrl[n].scale[IN][i] = fctrl[n].scale[IN][i];
        }
        for (i = 0; i < filters[n].n_filters[IN]; i++) {
            newstate.fctrl[n].fscale[i] = fctrl[n].fscale[i];
        }
    }
    memset(newstate.toggle_mute, 0, sizeof(newstate.toggle_mute));
    memset(newstate.fchanged, 0, sizeof(newstate.fchanged));
    memset(newstate.delay, -1, sizeof(newstate.delay));
    for (n = 0; n < BF_MAXCHANNELS; n++) {
        newstate.subdelay[IN][n] = BF_UNDEFINED_SUBDELAY;
        newstate.subdelay[OUT][n] = BF_UNDEFINED_SUBDELAY;
    }
}

static void
commit_changes(FILE *stream)
{
    int n, i;
    
    FOR_IN_AND_OUT {
        for (n = 0; n < n_channels[IO]; n++) {
            if (newstate.delay[IO][n] != -1) {
                if (bfaccess->set_delay(IO, n, newstate.delay[IO][n]) == -1) {
                    fprintf(stream, "Could not change %s delay.\n",
                            IO == IN ? "input" : "output");
                }
            }
            if (newstate.subdelay[IO][n] != BF_UNDEFINED_SUBDELAY) {
                if (bfaccess->set_subdelay(IO, n,
                                           newstate.subdelay[IO][n]) == -1)
                {
                    fprintf(stream, "Could not change %s subdelay.\n",
                            IO == IN ? "input" : "output");
                }
            }
            if (newstate.toggle_mute[IO][n]) {
                bfaccess->toggle_mute(IO, n);
            }
        }
    }
    for (n = 0; n < n_filters; n++) {
        if (!newstate.fchanged[n]) {
            continue;
        }
        fctrl[n].coeff = newstate.fctrl[n].coeff;
        fctrl[n].delayblocks = newstate.fctrl[n].delayblocks;
        for (i = 0; i < filters[n].n_channels[OUT]; i++) {
            fctrl[n].scale[OUT][i] = newstate.fctrl[n].scale[OUT][i];
        }
        for (i = 0; i < filters[n].n_channels[IN]; i++) {
            fctrl[n].scale[IN][i] = newstate.fctrl[n].scale[IN][i];
        }
        for (i = 0; i < filters[n].n_filters[IN]; i++) {
            fctrl[n].fscale[i] = newstate.fctrl[n].fscale[i];
        }
    }
}

static bool_t
are_changes(void)
{
    int n;
    
    FOR_IN_AND_OUT {
        for (n = 0; n < n_channels[IO]; n++) {
            if (newstate.delay[IO][n] != -1 ||
                newstate.subdelay[IO][n] != BF_UNDEFINED_SUBDELAY ||
                newstate.toggle_mute[IO][n])
            {
                return true;
            }
        }
    }
    for (n = 0; n < n_filters; n++) {
        if (newstate.fchanged[n]) {
            return true;
        }
    }
    return false;
}

static void
print_overflows(FILE *stream)
{
    double peak;
    int n;
    
    fprintf(stream, "peak: ");
    for (n = 0; n < n_channels[OUT]; n++) {
	peak = (volatile double)bfaccess->overflow[n].largest;
        if (peak < (double)(volatile int32_t)bfaccess->overflow[n].intlargest) {
            peak = (double)(volatile int32_t)bfaccess->overflow[n].intlargest;
        }
	if (peak != 0.0) {
	    if ((peak = 20.0 * log10(peak / bfaccess->overflow[n].max))
                == 0.0)
	    {                
		peak = -0.0;
	    }
            fprintf(stream, "%d/%u/%+.2f ", n,
                    (volatile unsigned int)bfaccess->overflow[n].n_overflows,
                    peak);
	} else {
            fprintf(stream, "%d/%u/-Inf ", n,
                    (volatile unsigned int)bfaccess->overflow[n].n_overflows);
        }
    }
    fprintf(stream, "\n");
}

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

static bool_t
get_id(FILE *stream,
       char str[],
       char **p,
       int *id,
       int type,
       int rid)
{
    int io = -1, n;

    str = strtrim(str);
    if (str[0] == '\"') {
	str += 1;
	if ((*p = strchr(str, '\"')) == NULL) {
	    fprintf(stream, "Invalid string.\n");
	    return false;
	}
	**p = '\0';
	switch (type) {
	case FILTER_ID:
	    io = -2;
	    for (*id = 0; *id < n_filters; (*id)++) {
		if (strcmp(filters[*id].name, str) == 0) {
		    break;
		}
	    }
	    if (*id == n_filters) {
		fprintf(stream, "There is no filter with name \"%s\".\n", str);
		return false;
	    }
	    break;
	case COEFF_ID:
	    for (*id = 0; *id < n_coeffs; (*id)++) {
		if (strcmp(coeffs[*id].name, str) == 0) {
		    break;
		}
	    }
	    if (*id == n_coeffs) {
		fprintf(stream, "There is no coefficient set with name "
			"\"%s\".\n", str);
		return false;
	    }
	    break;	    
	case INPUT_ID:
	case OUTPUT_ID:
	    io = (type == INPUT_ID) ? IN : OUT;
	    for (*id = 0; *id < n_channels[io]; (*id)++) {
		if (strcmp(channels[io][*id].name, str) == 0) {
		    break;
		}
	    }
	    if (*id == n_channels[io]) {
		fprintf(stream, "There is no %s with name \"%s\".\n",
			(io == IN) ? "input" : "output", str);
		return false;
	    }
	    break;
	}
    } else {
	*id = strtol(str, p, 10);
	if (*p == str) {
	    fprintf(stream, "Invalid number.\n");
	    return false;
	}
	if (*id < 0 && type != COEFF_ID) {
	    fprintf(stream, "Negative number (%d) is not allowed.\n", *id);
	    return false;
	}
	switch (type) {	    
	case FILTER_ID:
	    io = -2;
	    if (*id >= n_filters) {
		fprintf(stream, "Filter id %d is out of range.\n", *id);
		return false;
	    }
	    break;
	case COEFF_ID:
	    if (*id >= (int)n_coeffs) {
		fprintf(stream, "Coefficient set id %d is out of range.\n",
			*id);
		return false;
	    }
	    break;
	case INPUT_ID:
	case OUTPUT_ID:
	    io = (type == INPUT_ID) ? IN : OUT;
	    if (*id >= n_channels[io]) {
		fprintf(stream, "%s id %d is out of range.\n",
			(io == IN) ? "Input" : "Output", *id);
		return false;
	    }
	    break;	    
	}
    }
    if (io != -1 && rid != -1) {
	if (io == -2) {
	    for (n = 0; n < filters[rid].n_filters[IN]; n++) {
		if (filters[rid].filters[IN][n] == *id) {
		    break;
		}
	    }
	    if (n == filters[rid].n_filters[IN]) {
		fprintf(stream, "Filter id %d does not exist in the given "
			"filter.\n", *id);
		return false;
	    }
	} else {
	    for (n = 0; n < filters[rid].n_channels[io]; n++) {
		if (filters[rid].channels[io][n] == *id) {
		    break;
		}
	    }
	    if (n == filters[rid].n_channels[io]) {
		fprintf(stream, "%s id %d does not exist in the given "
			"filter.\n", (io == IN) ? "Input" : "Output", *id);
		return false;
	    }
	}
	*id = n;
    }
    *p += 1;
    while (**p == ' ' || **p == '\t') {
        (*p)++;
    }
    return true;
}

static bool_t
parse_command(FILE *stream,
	      char cmd[],
              struct sleep_task *_sleep_task)
{
    struct sleep_task sleep_task;
    int n, i, rid, id, range[2];
    const char **names;
    double att;
    char *p;

    if (strcmp(cmd, "lf") == 0) {
	fprintf(stream, "Filters:\n");
	for (n = 0; n < n_filters; n++) {
	    fprintf(stream, "  %d: \"%s\"\n", n, filters[n].name);
	    if (fctrl[n].coeff < 0) {
		fprintf(stream, "      coeff set: %d (no filter)\n",
			fctrl[n].coeff);
	    } else {
		fprintf(stream, "      coeff set: %d\n", fctrl[n].coeff);
	    }
	    fprintf(stream, "      delay blocks: %d (%d samples)\n",
		    fctrl[n].delayblocks, fctrl[n].delayblocks * block_length);
	    FOR_IN_AND_OUT {
		fprintf(stream, (IO == IN) ? "      from inputs:  " :
			"      to outputs:   ");
		for (i = 0; i < filters[n].n_channels[IO]; i++) {
                    if (fctrl[n].scale[IO][i] < 0) {
                        att = -20.0 * log10(-fctrl[n].scale[IO][i]);
                    } else {
                        att = -20.0 * log10(fctrl[n].scale[IO][i]);
                    }
		    if (att == 0.0) {
			att = 0.0000001; /* to show up as 0.0 and not -0.0 */
		    }
		    fprintf(stream, "%d/%.1f", filters[n].channels[IO][i],
			    att);
                    if (fctrl[n].scale[IO][i] < 0) {
                        fprintf(stream, "/-1 ");
                    } else {
                        fprintf(stream, " ");
                    }
		}
		fprintf(stream, "\n");
	    }
	    FOR_IN_AND_OUT {
		fprintf(stream, (IO == IN) ? "      from filters: " :
			"      to filters:   ");
		for (i = 0; i < filters[n].n_filters[IO]; i++) {
		    if (IO == IN) {                        
                        if (fctrl[n].fscale[i] < 0) {
                            att = -20.0 * log10(-fctrl[n].fscale[i]);
                        } else {
                            att = -20.0 * log10(fctrl[n].fscale[i]);
                        }
			if (att == 0.0) {
			    att = 0.0000001;
			}
			fprintf(stream, "%d/%.1f", filters[n].filters[IO][i],
				att);
                        if (fctrl[n].fscale[i] < 0) {
                            fprintf(stream, "/-1 ");
                        } else {
                            fprintf(stream, " ");
                        }
		    } else {
			fprintf(stream, "%d ",
				filters[n].filters[IO][i]);
		    }
		}
		fprintf(stream, "\n");
	    }
	}
	fprintf(stream, "\n");
    } else if (strcmp(cmd, "lc") == 0) {
	fprintf(stream, "Coefficient sets:\n");
	for (n = 0; n < n_coeffs; n++) {
	    fprintf(stream, "  %d: \"%s\" (%d blocks)\n", n,
		    coeffs[n].name,
		    coeffs[n].n_blocks);
	}
	fprintf(stream, "\n");
    } else if (strcmp(cmd, "li") == 0) {
	fprintf(stream, "Input channels:\n");
	for (n = 0; n < n_channels[IN]; n++) {
	    fprintf(stream, "  %d: \"%s\" (delay: %d:%d) %s\n", n,
		    channels[IN][n].name, bfaccess->get_delay(IN, n),
                    bfaccess->get_subdelay(IN, n),
		    bfaccess->ismuted(IN, n) ? "(muted)" : "");
	}
	fprintf(stream, "\n");
    } else if (strcmp(cmd, "lo") == 0) {
	fprintf(stream, "Output channels:\n");
	for (n = 0; n < n_channels[OUT]; n++) {
	    fprintf(stream, "  %d: \"%s\" (delay: %d:%d) %s\n", n,
		    channels[OUT][n].name, bfaccess->get_delay(OUT, n),
                    bfaccess->get_subdelay(OUT, n),
		    bfaccess->ismuted(OUT, n) ? "(muted)" : "");
	}
	fprintf(stream, "\n");	
    } else if (strcmp(cmd, "lm") == 0) {
	names = bfaccess->bflogic_names(&i);
	if (names != NULL) {
	    fprintf(stream, "Logic modules:\n");
	    for (n = 0; n < i; n++) {
		fprintf(stream, "  %d: \"%s\"\n", n, names[n]);
	    }
	    fprintf(stream, "\n");	
	}
	FOR_IN_AND_OUT {
	    names = bfaccess->bfio_names(IO, &i);
	    if (names != NULL) {
		fprintf(stream, "%s modules:\n", (IO == IN) ? "Input" :
			"Output");
		for (n = 0; n < i; n++) {
		    bfaccess->bfio_range(IO, n, range);
		    fprintf(stream, "  %d (%d - %d): \"%s\"\n",
			    n, range[0], range[1], names[n]);
		}
		fprintf(stream, "\n");	
	    }
	}
    } else if (strstr(cmd, "cffa") == cmd) {
	if (get_id(stream, cmd + 4, &cmd, &rid, FILTER_ID, -1) &&
	    get_id(stream, cmd, &cmd, &id, FILTER_ID, rid))
	{
            if (*cmd == 'M' || *cmd == 'm') {
                cmd++;
                att = strtod(cmd, &p);
                if (cmd == p) {
                    fprintf(stream, "Invalid input multiplier.\n");
                } else {
                    newstate.fctrl[rid].fscale[id] = att;
                    newstate.fchanged[rid] = true;
                }
            } else {
                att = strtod(cmd, &p);
                if (cmd == p) {
                    fprintf(stream, "Invalid input attenuation.\n");
                } else {
                    if (newstate.fctrl[rid].fscale[id] < 0) {
                        newstate.fctrl[rid].fscale[id] = -pow(10, -att / 20);
                    } else {
                        newstate.fctrl[rid].fscale[id] = pow(10, -att / 20);
                    }
                    newstate.fchanged[rid] = true;
                }
            }
	}
    } else if (strstr(cmd, "cfia") == cmd) {
	if (get_id(stream, cmd + 4, &cmd, &rid, FILTER_ID, -1) &&
	    get_id(stream, cmd, &cmd, &id, INPUT_ID, rid))
	{
            if (*cmd == 'M' || *cmd == 'm') {
                cmd++;
                att = strtod(cmd, &p);
                if (cmd == p) {
                    fprintf(stream, "Invalid input multiplier.\n");
                } else {
                    newstate.fctrl[rid].scale[IN][id] = att;
                    newstate.fchanged[rid] = true;
                }
            } else {
                att = strtod(cmd, &p);
                if (cmd == p) {
                    fprintf(stream, "Invalid input attenuation.\n");
                } else {
                    if (newstate.fctrl[rid].scale[IN][id] < 0) {
                        newstate.fctrl[rid].scale[IN][id] = -pow(10, -att / 20);
                    } else {
                        newstate.fctrl[rid].scale[IN][id] = pow(10, -att / 20);
                    }
                    newstate.fchanged[rid] = true;
                }
            }
	}
    } else if (strstr(cmd, "cfoa") == cmd) {
	if (get_id(stream, cmd + 4, &cmd, &rid, FILTER_ID, -1) &&
	    get_id(stream, cmd, &cmd, &id, OUTPUT_ID, rid))
	{
            if (*cmd == 'M' || *cmd == 'm') {
                cmd++;
                att = strtod(cmd, &p);
                if (cmd == p) {
                    fprintf(stream, "Invalid output multiplier.\n");
                } else {
                    newstate.fctrl[rid].scale[OUT][id] = att;
                    newstate.fchanged[rid] = true;
                }
            } else {
                att = strtod(cmd, &p);
                if (cmd == p) {
                    fprintf(stream, "Invalid output attenuation.\n");
                } else {
                    if (newstate.fctrl[rid].scale[OUT][id] < 0) {
                        newstate.fctrl[rid].scale[OUT][id] =
                            -pow(10, -att / 20);
                    } else {
                        newstate.fctrl[rid].scale[OUT][id] = pow(10, -att / 20);
                    }
                    newstate.fchanged[rid] = true;
                }
            }
	}
    } else if (strstr(cmd, "cid") == cmd) {
	if (get_id(stream, cmd + 3, &cmd, &id, INPUT_ID, -1)) {
	    n = strtol(cmd, &p, 10);
	    if (cmd == p || n < 0) {
		fprintf(stream, "Invalid input delay.\n");
	    } else {
                newstate.delay[IN][id] = n;
	    }
            cmd = p;
	    n = strtol(cmd, &p, 10);
            if (cmd != p) {
                if (n <= -BF_SAMPLE_SLOTS || n >= BF_SAMPLE_SLOTS) {
                    fprintf(stream, "Invalid input subdelay.\n");
                } else {
                    newstate.subdelay[IN][id] = n;
                }
            }
	}
    } else if (strstr(cmd, "cod") == cmd) {
	if (get_id(stream, cmd + 3, &cmd, &id, OUTPUT_ID, -1)) {
	    n = strtol(cmd, &p, 10);
	    if (cmd == p || n < 0) {
		fprintf(stream, "Invalid output delay.\n");
	    } else {
                newstate.delay[OUT][id] = n;
	    }
            cmd = p;
	    n = strtol(cmd, &p, 10);
            if (cmd != p) {
                if (n <= -BF_SAMPLE_SLOTS || n >= BF_SAMPLE_SLOTS) {
                    fprintf(stream, "Invalid output subdelay.\n");
                } else {
                    newstate.subdelay[OUT][id] = n;
                }
            }
	}
    } else if (strstr(cmd, "cfc") == cmd) {
	if (get_id(stream, cmd + 3, &cmd, &rid, FILTER_ID, -1) &&
	    get_id(stream, cmd, &cmd, &id, COEFF_ID, rid))
	{
	    newstate.fctrl[rid].coeff = id;
            newstate.fchanged[rid] = true;
	}
    } else if (strstr(cmd, "cfd") == cmd) {
	if (get_id(stream, cmd + 3, &cmd, &rid, FILTER_ID, -1)) {
	    n = strtol(cmd, &p, 10);
	    if (cmd == p || n < 0 || n > n_maxblocks - 1) {
		fprintf(stream, "Invalid filter delay.\n");
	    } else {
		newstate.fctrl[rid].delayblocks = n;
                newstate.fchanged[rid] = true;
	    }
	}
    } else if (strstr(cmd, "tmo") == cmd) {
	if (get_id(stream, cmd + 3, &cmd, &id, OUTPUT_ID, -1)) {
            newstate.toggle_mute[OUT][id] = !newstate.toggle_mute[OUT][id];
	}
    } else if (strstr(cmd, "tmi") == cmd) {
	if (get_id(stream, cmd + 3, &cmd, &id, INPUT_ID, -1)) {
            newstate.toggle_mute[IN][id] = !newstate.toggle_mute[IN][id];
	}
    } else if (strstr(cmd, "imc") == cmd) {
	id = strtol(cmd + 3, &p, 10);
	if (p == cmd + 3) {
	    id = -1;
	}
	if (bfaccess->bfio_command(IN, id, p, &p) == -1) {
	    fprintf(stream, "Command failed: %s\n", p);
	} else {
	    fprintf(stream, "%s", p);
	}
    } else if (strstr(cmd, "omc") == cmd) {
	id = strtol(cmd + 3, &p, 10);
	if (p == cmd + 3) {
	    id = -1;
	}
	if (bfaccess->bfio_command(OUT, id, p, &p) == -1) {
	    fprintf(stream, "Command failed: %s\n", p);
	} else {
	    fprintf(stream, "%s", p);
	}
    } else if (strstr(cmd, "lmc") == cmd) {
	id = strtol(cmd + 3, &p, 10);
	if (p == cmd + 3) {
	    id = -1;
	    names = bfaccess->bflogic_names(&i);
	    if (names != NULL) {
		p = strtrim(cmd + 3);
		for (n = 0; n < i; n++) {
		    if (strstr(p, names[n]) == p) {
			id = n;
			p += strlen(names[n]);
			break;
		    }
		}
	    }
	}
	if (bfaccess->bflogic_command(id, p, &p) == -1) {
	    fprintf(stream, "Command failed: %s\n", p);
	} else {
	    fprintf(stream, "%s", p);
	}
    } else if (strcmp(cmd, "ppk") == 0) {
	print_overflows(stream);
    } else if (strcmp(cmd, "rpk") == 0) {
	bfaccess->reset_peak();
    } else if (strcmp(cmd, "upk") == 0) {
	print_peak_updates = !print_peak_updates;
    } else if (strcmp(cmd, "tp") == 0) {
	print_prompt = !print_prompt;
    } else if (strcmp(cmd, "rti") == 0) {
	fprintf(stream, "Realtime index: %.3f\n", bfaccess->realtime_index());
    } else if (strcmp(cmd, "quit") == 0) {
	return false;
    } else if (strstr(cmd, "sleep") == cmd) {
        memset(&sleep_task, 0, sizeof(struct sleep_task));
        if ((p = strchr(cmd + 5, 'b')) != NULL) {
            n = strtol(p + 1, &p, 10);
            if (p != NULL && n >= 0) {
                sleep_task.block_sleep = true;
                sleep_task.do_sleep = true;
                sleep_task.blocks = n;
            }
        } else {
            n = strtol(cmd + 5, &p, 10);
            if (p != NULL && n >= 0) {
                sleep_task.do_sleep = true;
                sleep_task.seconds = n;
                sleep_task.useconds = atoi(p) * 1000;
            }
        }
        if (sleep_task.do_sleep) {
            if (_sleep_task == NULL) {
                if (sleep_task.block_sleep) {
                    fprintf(stream, "Block sleep only valid in scripts\n");
                } else {
                    if (sleep_task.seconds > 0) {
                        sleep(sleep_task.seconds);
                    }
                    if (sleep_task.useconds > 0) {
                        usleep(sleep_task.useconds);
                    }
                }
            } else {
                memcpy(_sleep_task, &sleep_task, sizeof(struct sleep_task));
            }
        }
    } else if (strstr(cmd, "abort") == cmd) {
	bfaccess->exit(BF_EXIT_OK);
    } else if (strcmp(cmd, "help") == 0) {
	fprintf(stream, HELP_TEXT);
    } else {
	fprintf(stream, "Unknown command \"%s\", type \"help\" for help.\n",
		cmd);
    }
    return true;
}

static void
wait_data(FILE *client_stream,
	  int client_fd,
	  int callback_fd)
{
    fd_set rfds;
    uint32_t msg;
    int n;

    FD_ZERO(&rfds);
    
    do {
	FD_SET(client_fd, &rfds);
	FD_SET(callback_fd, &rfds);
	if (client_stream != NULL) {
	    fflush(client_stream);
	}
	while ((n = select(client_fd < callback_fd ? callback_fd + 1 :
			   client_fd + 1, &rfds, NULL, NULL, NULL)) == -1
	       && errno == EINTR);
	if (n == -1) {
	    fprintf(stderr, "CLI: Select failed: %s.\n", strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}
	if (FD_ISSET(callback_fd, &rfds)) {
	    if (!readfd(callback_fd, &msg, 4)) {
                bfaccess->exit(BF_EXIT_OK);
            }
	    switch (msg) {
	    case BF_FDEVENT_PEAK:
		if (print_peak_updates) {
		    print_overflows(client_stream);
		}
		break;
	    default:
		fprintf(stderr, "CLI: Invalid callback code: %d.\n", msg);
		bfaccess->exit(BF_EXIT_OTHER);
		break;
	    }
	}
    } while (!FD_ISSET(client_fd, &rfds));
}

static bool_t
parse(FILE *stream,
      const char cmd[],
      struct sleep_task *sleep_task)
{
    char *p, *s, *s1, *buf;
    bool_t do_quit;
    int len;

    if (sleep_task != NULL) {
        memset(sleep_task, 0, sizeof(struct sleep_task));
    }
    do_quit = false;
    len = strlen(cmd);
    if (len == 0) {
        return true;
    }
    buf = alloca(len + 1);
    memcpy(buf, cmd, len + 1);
    if (buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
    if (len > 1 && buf[len - 2] == '\r') {
        buf[len - 2] = '\0';
    }
    s = buf;
    clear_changes();
    do {
        if ((p = strchr(s, ';')) != NULL) {
            *p = '\0';
        }
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '\0') {
            s1 = strtrim(s);
            if (print_commands) {
                fprintf(stream, "%s\n", s1);
            }
            if (!parse_command(stream, s1, sleep_task)) {
                do_quit = true;
            }
        } else if (are_changes()) {
            bfaccess->control_mutex(1);
            commit_changes(stream);
            bfaccess->control_mutex(0);
            clear_changes();
        }
    } while ((s = p + 1) != (char *)1);
    if (are_changes()) {
        bfaccess->control_mutex(1);
        commit_changes(stream);
        bfaccess->control_mutex(0);
    }    
    return !do_quit;
}

static void
block_start(struct bfaccess *_bfaccess,
            unsigned int block_index,
            struct timeval *current_time)
{
    static char *p = NULL, *p1, *p2;
    static bool_t do_quit = false, do_sleep = false, sleep_block;
    static unsigned int sleep_block_index;
    static struct timeval sleep_time;
    static bool_t retchr, cmdchr;
    static char restore_char;

    struct sleep_task sleep_task;
    struct timeval tv;

    bfaccess = _bfaccess;
    fctrl = _bfaccess->fctrl;
    if (do_quit) {
        return;
    }
    if (do_sleep) {
        if (sleep_block) {
            if (block_index <= sleep_block_index) {
                return;
            }
        } else {
            if (timercmp(current_time, &sleep_time, <)) {
                return;
            }
        }
        do_sleep = false;
    }
    if (p == NULL) {
        p = script;
    }
    cmdchr = false;
    retchr = false;
    p1 = p;
    /* this loop extracts the next non-empty line, and handles wrap */
    while ((*p1 != '\0' && *p1 != '\n') || !cmdchr) {
        switch ((int)*p1) {
        case ' ': case '\t': case '\r':
            break;
        case '\n':
            p = &p1[1];
            break;
        case '\0':
            if (p == script) {
                fprintf(stderr, "CLI: the script is empty.\n");
                bfaccess->exit(BF_EXIT_INVALID_CONFIG);
            }
            p = script;
            p1 = &p[-1];
            break;
        default:
            cmdchr = true;
            break;
        }
        p1++;
    }
    /* search for empty statements on the line */
    cmdchr = false;
    p2 = p;
    while (p2 < p1) {
        switch ((int)*p2) {
        case ';':
            if (!cmdchr) {
                /* empty statement, treat as linebreak */
                p1 = p2;
            }
            cmdchr = false;
            break;
        case ' ': case '\t': case '\r':
            break;
        default:
            cmdchr = true;
            break;
        }
        p2++;
    }
    /* handle \r\n line ends */
    restore_char = *p1;
    *p1 = '\0';
    if (p1 != script && p1[-1] == '\r') {
        retchr = true;
        p1[-1] = '\0';
    }

    if (!parse(stderr, p, &sleep_task)) {
        do_quit = true;
    }

    if (retchr) {
        p1[-1] = '\r';
    }
    *p1 = restore_char;
    if (*p1 == '\0') {
        /* wrap */
        p = script;
    } else {
        p = &p1[1];
    }

    if (sleep_task.do_sleep) {
        do_sleep = true;
        if (sleep_task.block_sleep) {
            sleep_block = true;
            sleep_block_index = block_index + sleep_task.blocks;
        } else {
            sleep_block = false;
            tv.tv_sec = sleep_task.seconds;
            tv.tv_usec = sleep_task.useconds;
            timeradd(current_time, &tv, &sleep_time);
        }
    }
}

static void
parse_string(FILE *stream,
             const char inbuf[MAXCMDLINE],
             char cmd[MAXCMDLINE])
{
    int slen, n, i;
    
    slen = strlen(inbuf);
    cmd[0] = '\0';
    for (n = 0, i = 0; n < slen; n++) {
        switch ((int)inbuf[n]) {
        case 27:
            fprintf(stream, "ESC sequences not supported, "
                    "discarding line.\n");
            cmd[0] = '\0';
            return;
        case '\b':
            if (i > 0) {
                i--;
            }
            break;
        default:
            if (inbuf[n] == '\n' || inbuf[n] == '\r' ||
                (inbuf[n] > 31 && inbuf[n] < 127))
            {
                cmd[i++] = inbuf[n];
            } else {
                fprintf(stream, "unsupported character in input (%u), "
                        "discarding line.\n", (unsigned int)inbuf[n]);
                cmd[0] = '\0';
                return;
            }
            break;
        }
    }
    cmd[i] = '\0';
}

static void
stream_loop(int event_fd,
            int infd,
            FILE *instream,
            FILE *outstream)
{
    char inbuf[MAXCMDLINE], cmd[MAXCMDLINE];
    
    inbuf[MAXCMDLINE - 1] = '\0';	
    while (true) {
        wait_data(outstream, infd, event_fd);
        if (fgets(inbuf, MAXCMDLINE - 1, instream) == NULL) {
            fprintf(stderr, "CLI: fgets failed: %s.\n", strerror(errno));
            bfaccess->exit(BF_EXIT_OTHER);
        }
        parse_string(outstream, inbuf, cmd);
        parse(outstream, cmd, NULL);
    }
}

static void
socket_loop(int event_fd,
            int lsock)
{
    char inbuf[MAXCMDLINE], cmd[MAXCMDLINE];
    FILE *stream;
    int sock;
    
    while (true) {
	wait_data(NULL, lsock, event_fd);
	if ((sock = accept(lsock, NULL, NULL)) == -1) {
	    fprintf(stderr, "CLI: Accept failed: %s.\n", strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}
	if ((stream = fdopen(sock, "r+")) == NULL) {
	    fprintf(stderr, "CLI: fdopen failed: %s.\n", strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}
	setvbuf(stream, NULL, _IOLBF, 0);

	fprintf(stream, WELCOME_TEXT);
	fprintf(stream, PROMPT_TEXT);
	
	wait_data(stream, sock, event_fd);
	cmd[MAXCMDLINE - 1] = '\0';	
	while (fgets(inbuf, MAXCMDLINE - 1, stream) != NULL) {
            parse_string(stream, inbuf, cmd);
	    if (!parse(stream, cmd, NULL)) {
		break;
	    }
	    if (print_prompt) {
		fprintf(stream, PROMPT_TEXT);
	    }
	    wait_data(stream, sock, event_fd);
	}
	print_peak_updates = false;
	fclose(stream);
    }
}

int
bflogic_preinit(int *version_major,
                int *version_minor,
                int (*get_config_token)(union bflexval *lexval),
                int sample_rate,
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
    union bflexval lexval;
    int token, ver;

    ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return -1;
    }
    memset(&newstate, 0, sizeof(newstate));
    error[0] = '\0';
    port = -1;
    port2 = -1;
    lport = NULL;
    script = NULL;
    line_speed = 9600;
    debug = !!_debug;

    while ((token = get_config_token(&lexval)) > 0) {
        if (token != BF_LEXVAL_FIELD) {
            fprintf(stderr, "CLI: Parse error: expected field.\n");
            return -1;
        }
        if (strcmp(lexval.field, "port") == 0) {
            switch (get_config_token(&lexval)) {
            case BF_LEXVAL_STRING:
                lport = strdup(lexval.string);
                break;
            case BF_LEXVAL_REAL:
                port = (int)lexval.real;
                switch (get_config_token(&lexval)) {
                case BF_LEX_COMMA:
                    if (get_config_token(&lexval) != BF_LEXVAL_REAL) {
                        fprintf(stderr,
                                "CLI: Parse error: expected integer.\n");
                        return -1;
                    }
                    port2 = (int)lexval.real;
                    break;
                case BF_LEX_EOS:
                    continue;
                default:
                    fprintf(stderr, "CLI: Parse error: expected end of "
                            "statement (;).\n");
                    return -1;
                }
                break;
            default:
                fprintf(stderr, "CLI: Parse error: expected string or "
                        "integer.\n");
                return -1;
            }
        } else if (strcmp(lexval.field, "script") == 0) {
            if (get_config_token(&lexval) != BF_LEXVAL_STRING) {
                fprintf(stderr, "CLI: Parse error: expected string.\n");
                return -1;
            }
            script = strdup(lexval.string);
        } else if (strcmp(lexval.field, "echo") == 0) {
            if (get_config_token(&lexval) != BF_LEXVAL_BOOLEAN) {
                fprintf(stderr, "CLI: Parse error: expected boolean.\n");
                return -1;
            }
            print_commands = lexval.boolean;
        } else if (strcmp(lexval.field, "line_speed") == 0) {
            if (get_config_token(&lexval) != BF_LEXVAL_REAL) {
                fprintf(stderr, "CLI: Parse error: expected integer.\n");
                return -1;
            }
            line_speed = (int)lexval.real;
        } else {
            fprintf(stderr, "CLI: Parse error: unknown field \"%s\".\n",
                    lexval.field);
            return -1;
        }
        if (get_config_token(&lexval) != BF_LEX_EOS) {
            fprintf(stderr, "CLI: Parse error: expected end of "
                    "statement (;).\n");
            return -1;
        }
    }
    
    n_coeffs = _n_coeffs;
    n_maxblocks = _n_maxblocks;
    block_length = _block_length;
    coeffs = _coeffs;
    n_channels = _n_channels;
    channels = _channels;
    n_filters = _n_filters;
    filters = _filters;

    if (script == NULL) {
        if (port == -1 && lport == NULL) {
            fprintf(stderr, "CLI: \"port\" or \"script\" must be set.\n");
            return -1;
        }    
        bfevents->fdevents = BF_FDEVENT_PEAK;
        *fork_mode = BF_FORK_PRIO_MAX;
    } else {
        if (port != -1 || lport != NULL) {
            fprintf(stderr, "CLI: Cannot have both \"script\" and \"port\" "
                    "set.\n");
            return -1;
        }    
        bfevents->block_start = block_start;
        *fork_mode = BF_FORK_DONT_FORK;
    }
    return 0;
}

#define WRITE_TO_SYNCH_FD                                                      \
    dummy = 0;                                                                 \
    if (!writefd(synch_fd, &dummy, 1)) {                                       \
        bfaccess->exit(BF_EXIT_OTHER);                                         \
    }                                                                          \
    close(synch_fd);

int
bflogic_init(struct bfaccess *_bfaccess,
	     int sample_rate,
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
    FILE *stream, *instream, *outstream;
    int lsock, fd, speed, opt;
    struct sockaddr_in s_in;
    struct sockaddr_un s_un;
    struct termios newtio;
    uint8_t dummy;

    bfaccess = _bfaccess;
    fctrl = _bfaccess->fctrl;
    
    if (script != NULL) {
        return 0;
    }

    if (lport != NULL && strstr(lport, "/dev/") == lport) {
        /* serial line interface */
        if ((fd = open(lport, O_RDWR | O_NOCTTY)) == -1) {
            fprintf(stderr, "CLI: Failed to open serial device: %s.\n",
                    strerror(errno));
            bfaccess->exit(BF_EXIT_OTHER);
        }
        speed = B9600;
        switch (line_speed) {
        case 0: speed = B9600; break;
        case 1200: speed = B1200; break;
        case 2400: speed = B2400; break;
        case 4800: speed = B4800; break;
        case 9600: speed = B9600; break;
        case 19200: speed = B19200; break;
        case 38400: speed = B38400; break;
        case 57600: speed = B57600; break;
        case 115200: speed = B115200; break;
        case 230400: speed = B230400; break;
        default:
            fprintf(stderr, "CLI: Invalid/unsupported serial line speed %d.\n",
                    speed);
            bfaccess->exit(BF_EXIT_OTHER);
            return 0;
        }
        memset(&newtio, 0, sizeof(newtio));
        newtio.c_cflag = CS8 | CLOCAL | CREAD;
        cfsetispeed(&newtio, speed);
        cfsetospeed(&newtio, speed);
        newtio.c_iflag = IGNPAR | ICRNL | ISTRIP;
        newtio.c_oflag = OPOST | ONLCR;
        newtio.c_lflag = ICANON;
#ifdef _POSIX_VDISABLE
        {
            int _i, _n;
            _i = sizeof(newtio.c_cc);
            _i /= sizeof(newtio.c_cc[0]);
            for (_n = 0; _n < _i; _n++) {
                newtio.c_cc[_n] = _POSIX_VDISABLE;
            }
        }
#endif    
        if (tcflush(fd, TCIFLUSH) == -1) {
            fprintf(stderr, "CLI: tcflush failed: %s.\n", strerror(errno));
            bfaccess->exit(BF_EXIT_OTHER);
        }
        if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
            fprintf(stderr, "CLI: tcsetattr failed: %s.\n", strerror(errno));
            bfaccess->exit(BF_EXIT_OTHER);
        }
        if ((stream = fdopen(fd, "r+")) == NULL) {
            fprintf(stderr, "CLI: fdopen failed: %s.\n", strerror(errno));
            bfaccess->exit(BF_EXIT_OTHER);
        }
        setvbuf(stream, NULL, _IOLBF, 0);
        WRITE_TO_SYNCH_FD;
        
        stream_loop(event_fd, fd, stream, stream);
        
    } else if (port != -1 && port2 != -1) {
        /* pipe interface */
	if ((instream = fdopen(port, "r")) == NULL) {
	    fprintf(stderr, "CLI: fdopen 'r' on fd %d failed: %s.\n",
                    port, strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}
	setvbuf(instream, NULL, _IOLBF, 0);
	if ((outstream = fdopen(port2, "w")) == NULL) {
	    fprintf(stderr, "CLI: fdopen 'w' on fd %d failed: %s.\n",
                    port2, strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}
	setvbuf(outstream, NULL, _IOLBF, 0);
        WRITE_TO_SYNCH_FD;

        stream_loop(event_fd, port, instream, outstream);

    } else if (port != -1) {
        /* TCP interface */
	memset(&s_in, 0, sizeof(s_in));
	s_in.sin_family = AF_INET;
	s_in.sin_addr.s_addr = INADDR_ANY;
	s_in.sin_port = htons(port);

	if ((lsock = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
	    fprintf(stderr, "CLI: Failed to create socket: %s.\n",
                    strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}
        opt = 1;
	if (setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))
	    == -1)
	{
	    fprintf(stderr, "CLI: Failed to set socket options: %s.\n",
		    strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}    
	if (bind(lsock, (struct sockaddr *)&s_in, sizeof(struct sockaddr_in))
	    == -1)
	{
	    fprintf(stderr, "CLI: Failed to bind name to socket: %s.\n",
		    strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}
	if (listen(lsock, 1) != 0) {
	    fprintf(stderr, "CLI: Failed to listen on port %d: %s.\n",
		    port, strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}
        WRITE_TO_SYNCH_FD;
        
        socket_loop(event_fd, lsock);
        
    } else if (lport != NULL) {
        /* local socket interface */
        remove(lport);
	memset(&s_un, 0, sizeof(s_un));
	s_un.sun_family = AF_UNIX;
	strncpy(s_un.sun_path, lport, sizeof(s_un.sun_path));
	s_un.sun_path[sizeof(s_un.sun_path) - 1] = '\0';

	if ((lsock = socket(PF_UNIX, SOCK_STREAM, 0)) == -1) {
	    fprintf(stderr, "CLI: Failed to create socket: %s.\n",
                    strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}

	if (bind(lsock, (struct sockaddr *)&s_un, sizeof(struct sockaddr_un))
	    == -1)
	{
            if (errno == EADDRINUSE) {
                fprintf(stderr, "CLI: Failed to create local socket: "
                        "path \"%s\" already exists.\n", s_un.sun_path);
            } else {
                fprintf(stderr, "CLI: Failed to bind name to socket: %s.\n",
                        strerror(errno));
            }
	    bfaccess->exit(BF_EXIT_OTHER);
	}
	if (listen(lsock, 1) != 0) {
	    fprintf(stderr, "CLI: Failed to listen on local "
                    "socket \"%s\": %s.\n", s_un.sun_path, strerror(errno));
	    bfaccess->exit(BF_EXIT_OTHER);
	}	
	free(lport);
        WRITE_TO_SYNCH_FD;
        
        socket_loop(event_fd, lsock);
        
    } else {
        fprintf(stderr, "CLI: No port specified.\n");
        bfaccess->exit(BF_EXIT_OTHER);
    }

    return 0;
}
