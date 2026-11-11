/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Hardware resource service.
 *
 * Maintains a database of resources which can be requested by other processes.
 */
#include <errno.h>
#include <hwres.h>
#include <sys/sysmsg.h>
#include "root-process.h"


int hwres_handler(message_context_t *ctp,
                  int code,
                  unsigned flags,
                  void *handle);
void hwres_seed(void);

/* Globals */
pthread_mutex_t g_rsrc_mutex = PTHREAD_MUTEX_INITIALIZER;
rsrc_root_node_t *g_rsrc_root;

/* Initialize the resource database service.
 *
 * Clears the root list, seeds the database from the system page, and registers
 * the resource-database message handler with the dispatcher.
 */
void hwres_init()
{
    /* Initialize all the list heads to NULL */
    g_rsrc_root = NULL;

    hwres_seed();

    /* Tell proc to send resource database messages our way */
    message_attach(dpp, NULL, _HWRES_BASE, _HWRES_MAX, hwres_handler, NULL);
}


/* Called to destroy a process's resource allocations */
void hwres_destroy_process(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    hwres_process_state_t *pidrsrc;

    if (!(pidrsrc = tmp->hwres_state)) {
        return;
    }
    /* Release the resources held & allocated by this process */
    if (pidrsrc->haversrc) {
        (void) _hwres_pid_clear(pid);
    }
    /* Get rid of the resource pointer itself */
    if (tmp->hwres_state) {
        _sfree(tmp->hwres_state, sizeof(hwres_process_state_t));
        tmp->hwres_state = NULL;
    }

    pthread_mutex_unlock(&g_rsrc_mutex);
}
