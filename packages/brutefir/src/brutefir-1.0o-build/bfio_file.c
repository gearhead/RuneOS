/*
 * (c) Copyright 2001 - 2004, 2006 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

#define IS_BFIO_MODULE
#include "bfmod.h"
#include "bit.h"

#define TEXT_BUFFER_SIZE 4096
#define OUTTEXT_FORMAT "%+.16e"

struct readstate {
    off_t filesize;
    off_t skipbytes;
    off_t curpos;
    bool_t loop;
    bool_t use_text;
    struct {
        int dupfd;
        int filefd;
        int bufsize;
        int offset;
        int parse_offset;
        char *buffer;
    } text;
};

struct writestate {
    int open_channels;
    bool_t use_text;
    struct {
        int bufsize;
        int offset;
        int print_offset;
        int partial_offset;
        char *buffer;
        char *partial_buffer;
    } text;
};

static struct readstate *readstate[FD_SETSIZE];
static struct writestate *writestate[FD_SETSIZE];
static fd_set fds[2];
static int fdmax[2] = { -1, -1 };
static bool_t debug = false;
static int outtext_len;
static int read_ready_fd = -1;

struct settings {
    off_t skipbytes;
    bool_t append;
    bool_t loop;
    bool_t text;
    char *path;
};

#define GET_TOKEN(token, errstr)                                               \
    if (get_config_token(&lexval) != token) {                                  \
        fprintf(stderr, "File I/O: Parse error: " errstr);                     \
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
    int token, ver, dummypipe[2];

    ver = *version_major;
    *version_major = BF_VERSION_MAJOR;
    *version_minor = BF_VERSION_MINOR;
    if (ver != BF_VERSION_MAJOR) {
        return NULL;
    }
    debug = !!_debug;
    settings = malloc(sizeof(struct settings));
    memset(settings, 0, sizeof(struct settings));
    while ((token = get_config_token(&lexval)) > 0) {
        if (token == BF_LEXVAL_FIELD) {
            if (strcmp(lexval.field, "path") == 0) {
                if (settings->path != NULL) {
                    fprintf(stderr, "File I/O: Parse error: path "
                            "already set.\n");
                    return NULL;
                }
                GET_TOKEN(BF_LEXVAL_STRING, "expected string.\n");
                settings->path = strdup(lexval.string);                
            } else if (strcmp(lexval.field, "skip") == 0) {
                GET_TOKEN(BF_LEXVAL_REAL, "expected integer.\n");
                settings->skipbytes = (off_t)lexval.real;
            } else if (strcmp(lexval.field, "append") == 0) {
                if (io == BF_IN) {
                    fprintf(stderr, "File I/O: Append on input makes "
                            "no sense.\n");
                    return NULL;
                }
                GET_TOKEN(BF_LEXVAL_BOOLEAN, "expected boolean value.\n");
                settings->append = lexval.boolean;
            } else if (strcmp(lexval.field, "loop") == 0) {
                if (io == BF_OUT) {
                    fprintf(stderr, "File I/O: Loop on output makes "
                            "no sense.\n");
                    return NULL;
                }
                GET_TOKEN(BF_LEXVAL_BOOLEAN, "expected boolean value.\n");
                settings->loop = lexval.boolean;
            } else if (strcmp(lexval.field, "text") == 0) {
                GET_TOKEN(BF_LEXVAL_BOOLEAN, "expected boolean value.\n");
                settings->text = lexval.boolean;
            } else {
                fprintf(stderr, "File I/O: Parse error: unknown field.\n");
                return NULL;
            }
            GET_TOKEN(BF_LEX_EOS, "expected end of statement (;).\n");
        } else {
            fprintf(stderr, "File I/O: Parse error: expected field.\n");
            return NULL;
        }
    }
    if (settings->path == NULL) {
        fprintf(stderr, "File I/O: Parse error: path not set.\n");
        return NULL;
    }
    if (settings->text) {
        if (read_ready_fd == -1) {
            /* make read file descriptor that immediately causes a
               return if used in select() */
            if (pipe(dummypipe) == -1) {
                fprintf(stderr, "File I/O: Could not create pipe: %s.\n",
                        strerror(errno));
                return NULL;
            }
            close(dummypipe[1]);
            read_ready_fd = dummypipe[0];
        }
        if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
#ifdef __LITTLE_ENDIAN__
            *sample_format = BF_SAMPLE_FORMAT_FLOAT64_LE;
#endif
#ifdef __BIG_ENDIAN__
            *sample_format = BF_SAMPLE_FORMAT_FLOAT64_BE;
#endif
        }
        switch (*sample_format) {
#ifdef __LITTLE_ENDIAN__
        case BF_SAMPLE_FORMAT_FLOAT64_LE:
#endif
#ifdef __BIG_ENDIAN__
        case BF_SAMPLE_FORMAT_FLOAT64_BE:
#endif
            break;
        default:
            fprintf(stderr, "File I/O: No support for text conversion of given"
                    " sample format.\n");
            return NULL;
        }
    } else {
        if (*sample_format == BF_SAMPLE_FORMAT_AUTO) {
            fprintf(stderr, "File I/O: No support for AUTO sample format.\n");
            return NULL;
        }
    }
    *uses_sample_clock = 0;
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
    struct writestate *ws;
    struct readstate *rs;
    struct stat buf;
    int fd, mode;

    settings = (struct settings *)params;
    *device_period_size = 0; 
    *isinterleaved = 1;
    if (io == BF_IN) {
	if ((fd = open(settings->path, O_RDONLY | O_NONBLOCK |
		       O_LARGEFILE)) == -1)
	{
	    fprintf(stderr, "File I/O: Could not open file \"%s\" for "
                    "reading: %s.\n", settings->path, strerror(errno));
	    return -1;
	}
        rs = malloc(sizeof(struct readstate));
        memset(rs, 0, sizeof(struct readstate));
        rs->filesize = 0;
        if (settings->loop) {
            if (stat(settings->path, &buf) != 0) {
                fprintf(stderr, "File I/O: Could not stat file \"%s\": %s.\n",
                        settings->path, strerror(errno));
                return -1;
            }
            rs->filesize = buf.st_size;
            if (rs->filesize == 0) {
                fprintf(stderr, "File I/O: Cannot loop empty file \"%s\".\n",
                        settings->path);
                return -1;
            }
        }
        rs->curpos = 0;
        rs->skipbytes = settings->skipbytes;
        rs->loop = settings->loop;
        rs->use_text = settings->text;
	if (settings->skipbytes > 0) {
	    if (lseek(fd, settings->skipbytes, SEEK_SET) == -1) {
		fprintf(stderr, "File seek failed.\n");
		return -1;
	    }
            rs->curpos = settings->skipbytes;
	}
        if (settings->text) {            
            rs->text.filefd = fd;
            rs->text.buffer = malloc(TEXT_BUFFER_SIZE + 1);
            rs->text.buffer[0] = '\0';
            rs->text.bufsize = TEXT_BUFFER_SIZE;
            if ((fd = dup(read_ready_fd)) == -1) {
                fprintf(stderr, "File I/O: Dup failed: %s.\n",
                        strerror(errno));
                return -1;
            }
            rs->text.dupfd = read_ready_fd;
        }
        readstate[fd] = rs;
    } else {
	if (settings->append) {
	    mode = O_APPEND;
	} else {
	    mode = O_TRUNC;
	}
	if ((fd = open(settings->path, O_WRONLY | O_CREAT | mode |
		       O_NONBLOCK | O_LARGEFILE, S_IRUSR | S_IWUSR |
		       S_IRGRP | S_IROTH)) == -1)
	{
	    fprintf(stderr, "File I/O: Could not create file \"%s\" for "
                    "writing: %s.\n", settings->path, strerror(errno));
	    return -1;
	}
        ws = malloc(sizeof(struct writestate));
        memset(ws, 0, sizeof(struct writestate));
        ws->open_channels = open_channels;
        ws->use_text = settings->text;
        if (settings->text) {
            ws->text.partial_buffer = malloc(open_channels * (outtext_len + 1));
            ws->text.bufsize = TEXT_BUFFER_SIZE;
            while (ws->text.bufsize < open_channels * (outtext_len + 1)) {
                ws->text.bufsize += TEXT_BUFFER_SIZE;
            }
            ws->text.buffer = malloc(ws->text.bufsize);
        }
        writestate[fd] = ws;
    }
    FD_SET(fd, &fds[io]);
    if (fd > fdmax[io]) {
        fdmax[io] = fd;
    }
    free(settings->path);
    free(settings);
    return fd;
}

static int
text_read(int fd,
          void *buf,
          int count)
{
    char *p, *p1, *parsebuf, *filebuf;
    int retval, i, rest, size;
    struct readstate *rs;
    double *a;

    rs = readstate[fd];
    a = (double *)buf;
    count >>= 3;
    i = 0;

    while (i < count) {
        parsebuf = &rs->text.buffer[rs->text.parse_offset];
        for (p = parsebuf;
             *p != '\n' && *p != ' ' && *p != '\t' && *p != '\0';
             p++);
        if (*p == '\0') {
            if (p != &rs->text.buffer[rs->text.offset]) {
                fprintf(stderr, "File I/O: Read failed: null character "
                        "in text input.\n");
                errno = EIO;
                return -1;
            }
            if (rs->text.offset == rs->text.bufsize) {
                /* move tail to start of buffer (normally 'rest' is <30 so it is
                   a cheap operation) */
                rest = rs->text.bufsize -
                    rs->text.parse_offset;
                if (rest > (rs->text.bufsize >> 3)) {
                    fprintf(stderr, "File I/O: Read failed: too long entry "
                            "in text input.\n");
                    errno = EIO;
                    return -1;
                }
                memcpy(rs->text.buffer, parsebuf, rest);
                rs->text.offset = rest;
                rs->text.parse_offset = 0;
            }
            filebuf = &rs->text.buffer[rs->text.offset];
            size = rs->text.bufsize - rs->text.offset;
            if ((retval = read(rs->text.filefd, filebuf, size)) == -1) {
                if (errno != EAGAIN && errno != EINTR) {
                    fprintf(stderr, "File I/O: Read failed: %s.\n",
                            strerror(errno));
                    return -1;
                }
                /* continue */
            }
            if (retval < size) {
                if (rs->text.dupfd != rs->text.filefd) {
                    if (dup2(rs->text.filefd, fd) == -1) {
                        fprintf(stderr, "File I/O: Dup2 failed: %s.\n",
                                strerror(errno));
                        errno = EINVAL;
                        return -1;
                    }
                    rs->text.dupfd = rs->text.filefd;
                }
                if (retval == -1) {
                    break;
                }
            }
            rs->curpos += retval;;
            if (rs->loop && rs->curpos == rs->filesize) {
                if (lseek(rs->text.filefd, rs->skipbytes, SEEK_SET) == -1) {
                    fprintf(stderr, "File I/O: seek failed: %s.\n",
                            strerror(errno));
                    return -1;
                }
                rs->curpos = rs->skipbytes;
            }
            if (retval == 0 && !rs->loop) {
                break;                    
            }
            rs->text.offset += retval;
            rs->text.buffer[rs->text.offset] = '\0';
            continue;
        }
        rs->text.parse_offset += p - parsebuf + 1;
        *p = '\0';
        while (*parsebuf == ' ' || *parsebuf == '\t') parsebuf++;
        if (parsebuf == p) {
            /* skip empty lines */
            continue;
        }
        a[i++] = strtod(parsebuf, &p1);
        if (p1 == parsebuf) {
            fprintf(stderr, "File I/O: Read failed: bad text format.\n");
            errno = EIO;
            return -1;
        }
    }
    if (i == count) {
        if (rs->text.dupfd != read_ready_fd) {
            if (dup2(read_ready_fd, fd) == -1) {
                fprintf(stderr, "File I/O: Dup2 failed: %s.\n",
                        strerror(errno));
                errno = EBADF;
                return -1;
            }
            rs->text.dupfd = read_ready_fd;
        }
    }
    return i << 3;
}

int
bfio_read(int fd,
	  void *buf,
	  int offset,
	  int count)
{
    int retval;

    if (readstate[fd]->use_text) {
        return text_read(fd, &((uint8_t *)buf)[offset], count);
    }
 retry:
    if ((retval = read(fd, &((uint8_t *)buf)[offset], count)) == -1) {
        if (errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "File I/O: Read failed: %s.\n",
                    strerror(errno));
        }
        return -1;
    }
    readstate[fd]->curpos += retval;;
    if (readstate[fd]->loop &&
        readstate[fd]->curpos == readstate[fd]->filesize)
    {
        if (lseek(fd, readstate[fd]->skipbytes, SEEK_SET) == -1) {
            fprintf(stderr, "File I/O: seek failed: %s.\n", strerror(errno));
            return -1;
        }
        readstate[fd]->curpos = readstate[fd]->skipbytes;
        if (retval == 0) {
            goto retry;
        }
    }
    return retval;
}

static int
text_write_helper(int fd,
                  int linelen)
{
    int retval, i, partial, whole;
    struct writestate *ws;
    struct iovec iovec[2];

    ws = writestate[fd];
    i = 0;
    if (ws->text.partial_offset != 0) {
        iovec[i].iov_base = &ws->text.partial_buffer[ws->text.partial_offset];
        iovec[i].iov_len = linelen - ws->text.partial_offset;
        i++;
    }
    if (ws->text.print_offset - ws->text.offset > 0) {
        iovec[i].iov_base = &ws->text.buffer[ws->text.offset];
        iovec[i].iov_len = ws->text.print_offset - ws->text.offset;
        i++;
    }
    if (i == 0) {
        return 0;
    }
    if ((retval = writev(fd, iovec, i)) == -1) {
        if (errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "File I/O: Write failed: %s.\n",
                    strerror(errno));
        }
        return -1;
    }
    partial = 0;
    if (ws->text.partial_offset != 0) {
        if (ws->text.partial_offset + retval >= linelen) {
            retval -= linelen - ws->text.partial_offset;
            ws->text.partial_offset = 0;
            partial = 1;
        } else {
            ws->text.partial_offset += retval;
            errno = EAGAIN;
            return -1;
        }
    }
    whole = retval / linelen;
    ws->text.partial_offset = retval % linelen;
    if (ws->text.partial_offset != 0) {
        memcpy(ws->text.partial_buffer, &ws->text.buffer[whole * linelen],
               linelen);
        retval += linelen - ws->text.partial_offset;
    }
    ws->text.offset += retval;
    if (ws->text.offset == ws->text.print_offset) {
        ws->text.print_offset = 0;
        ws->text.offset = 0;
    }
    return whole + partial;
}

static int
text_write(int fd,
	   const void *buf,
	   int count)
{
    int retval, i, j, k, linelen;
    struct writestate *ws;
    const double *a;
    char *printbuf;

    ws = writestate[fd];
    count >>= 3;
    a = (const double *)buf;
    if (ws->text.partial_offset != 0) {
        a += ws->open_channels;
        count -= ws->open_channels;
    }
    retval = 1;
    i = 0;
    j = 0;
    linelen = ws->open_channels * (outtext_len + 1);
    while (i < count) {
        if (ws->text.print_offset + linelen > ws->text.bufsize) {
            if ((retval = text_write_helper(fd, linelen)) <= 0) {
                if (retval == -1 && errno != EAGAIN && errno != EINTR) {
                    return -1;
                }
                break;
            }
            j += retval;
        } else {
            printbuf = &ws->text.buffer[ws->text.print_offset];
            for (k = 0; k < ws->open_channels; k++) {
                sprintf(printbuf, OUTTEXT_FORMAT, a[i++]);
                printbuf += outtext_len + 1;
                printbuf[-1] = '\t';
            }
            printbuf[-1] = '\n';
            ws->text.print_offset += linelen;
        }
    }
    if ((retval > 0) &&
        (ws->text.partial_offset != 0 || ws->text.print_offset != 0))
    {
        if ((retval = text_write_helper(fd, linelen)) <= 0) {
            if (retval == -1 && errno != EAGAIN && errno != EINTR) {
                return -1;
            }
            retval = 0;
        }
        j += retval;
    }
    if (j == 0) {
        errno = EAGAIN;
        return -1;
    }
    return ws->open_channels * (j << 3);
}

int
bfio_write(int fd,
	   const void *buf,
	   int offset,
	   int count)
{
    int retval;

    if (writestate[fd]->use_text) {
        return text_write(fd, &((const uint8_t *)buf)[offset], count);
    }
    if ((retval = write(fd, &((const uint8_t *)buf)[offset], count)) == -1) {
        if (errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "File I/O: Write failed: %s.\n", strerror(errno));
        }
    }
    return retval;
}

int
bfio_start(int io)
{
    /* do nothing */
    return 0;
}

void
bfio_stop(int io)
{
    int fd;
    
    for (fd = 0; fd <= fdmax[io]; fd++) {
        if (FD_ISSET(fd, &fds[io])) {
            close(fd);
        }
    }
}

void
do_init(void);
void __attribute__((constructor))
do_init(void)
{
    char s[1024];
    
    memset(readstate, 0, sizeof(readstate));
    memset(fds, 0, sizeof(fds));
    sprintf(s, OUTTEXT_FORMAT, 1.0);
    outtext_len = strlen(s);
}
