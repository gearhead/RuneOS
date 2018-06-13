/*
 * (c) Copyright 2001 - 2004 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#include "defs.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#ifdef __OS_LINUX__
#include <malloc.h>
#else
#include <stddef.h>
#endif

#include "shmalloc.h"

static void
print_shmget_error(size_t size)
{
    switch (errno) {
    case EINVAL:
        fprintf(stderr, "shmget() failed when requesting %d bytes.\n"
                "  The OS shmmax parameter must be increased.\n", (int)size);
        break;
    case ENOSPC:
        fprintf(stderr, "shmget() failed due to exceeded id limit.\n"
                "  The OS shmmni parameter must be increased.\n");
        break;
    case ENOMEM:
        fprintf(stderr, "shmget() failed when requesting %d bytes.\n"
                "Out of memory, or the OS parameters shmmax and/or shmmni "
                "may be too small.\n", (int)size);
        break;
    default:
        fprintf(stderr, "shmget() failed when requesting %d bytes: %s.\n",
                (int)size, strerror(errno));
        break;
    }
}

void *
shmalloc(size_t size)
{
    struct shmid_ds shmid_ds;
    void *p;
    int n;

    if ((n = shmget(IPC_PRIVATE, size, IPC_CREAT | SHM_R | SHM_W)) == -1) {
        print_shmget_error(size);
        return NULL;
    }
    memset(&shmid_ds, 0, sizeof(shmid_ds)); /* to get rid of valgrind warning */
    if ((p = shmat(n, 0, 0)) == (char *)-1 ||
        shmctl(n, IPC_RMID, &shmid_ds) == -1)
    {
	return NULL;
    }
    if (((ptrdiff_t)p & (ALIGNMENT - 1)) != 0) {
	fprintf(stderr, "alignment error\n");
    }
    return p;	
}

void *
shmalloc_id(int *shmid,
            size_t size)
{
    static key_t key = 1;
    struct shmid_ds shmid_ds;
    void *p;
    int n;

    if (shmid != NULL) {
        *shmid = -1;
    }
    
    while ((n = shmget(key, size, IPC_CREAT | IPC_EXCL |
                       SHM_R | SHM_W)) == -1 && errno == EEXIST)
    {
        key++;
    }
    if (n == -1) {
        print_shmget_error(size);
	return NULL;
    }
    
    if  ((p = shmat(n, 0, 0)) == (char *)-1) {
	fprintf(stderr, "Failed to attach to shared memory (shmid %d): %s.\n",
                n, strerror(errno));
	return NULL;
    }
    memset(&shmid_ds, 0, sizeof(shmid_ds));
    if (shmctl(n, IPC_RMID, &shmid_ds) == -1) {
	fprintf(stderr, "Failed to set IPC_RMID (shmid %d): %s.\n", n,
                strerror(errno));
	return NULL;
    }
    
    key++;

    if (shmid != NULL) {
        *shmid = n;
    }
    
    return p;
}
