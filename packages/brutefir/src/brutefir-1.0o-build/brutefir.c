/*
 * (c) Copyright 2001 - 2006, 2009, 2013, 2016 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "emalloc.h"
#include "bfconf.h"
#include "pinfo.h"
#include "bfmod.h"

#define PRESENTATION_STRING \
"\n\
BruteFIR v1.0o (November 2016)                                \
(c) Anders Torger\n\
\n"

#define USAGE_STRING \
"Usage: %s [-quiet] [-nodefault] [-daemon] [configuration file]\n"

int
main(int argc,
     char *argv[])
{
    char *config_filename = NULL;
    bool_t quiet = false;
    bool_t nodefault = false;
    bool_t run_as_daemon = false;
    int n;

    for (n = 1; n < argc; n++) {
	if (strcmp(argv[n], "-quiet") == 0) {
	    quiet = true;
	} else if (strcmp(argv[n], "-nodefault") == 0) {
            nodefault = true;
	} else if (strcmp(argv[n], "-daemon") == 0) {
            run_as_daemon = true;
	} else {
	    if (config_filename != NULL) {
		break;
	    }
	    config_filename = argv[n];
	}
    }
    if (n != argc) {
	fprintf(stderr, PRESENTATION_STRING);
	fprintf(stderr, USAGE_STRING, argv[0]);
	return BF_EXIT_INVALID_CONFIG;
    }
    
    if (!quiet) {
	fprintf(stderr, PRESENTATION_STRING);
    }
    
    emalloc_set_exit_function(bf_exit, BF_EXIT_NO_MEMORY);
    
    bfconf_init(config_filename, quiet, nodefault);

    if (run_as_daemon) {
        switch (fork()) {
        case 0:
            break;
        case -1:
            fprintf(stderr, "fork failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        default:
            exit(EXIT_SUCCESS);
        }
        if (setsid() == -1) {
            fprintf(stderr, "setsid failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (chdir("/") == -1) {
            fprintf(stderr, "chdir failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        umask(0);
    }
    
    /* start! */
    bfrun();

    fprintf(stderr, "Could not start filtering.\n");
    bf_exit(BF_EXIT_OTHER);
    return BF_EXIT_OTHER;
}
