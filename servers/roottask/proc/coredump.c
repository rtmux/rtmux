/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <stdio.h>
#include "externs.h"
#include "private.h"
#include "root-process.h"
#include <private/root-proc-info.h>

extern int _connect(int base,
                    const char *path,
                    mode_t mode,
                    unsigned oflag,
                    unsigned sflag,
                    unsigned subtype,
                    int testcancel,
                    unsigned access,
                    unsigned file_type,
                    unsigned extra_type,
                    unsigned extra_len,
                    const void *extra,
                    unsigned response_len,
                    void *response,
                    int *status);

/* proc_coredump — coredump notification handler.
 *
 * Asks /proc/dumper to write a core file for the dying process (honouring the
 * RLIMIT_CORE soft limit), clears RTMUX_FLG_PROC_COREDUMP if no dump was
 * produced, then lets the process terminate via process_destroy_all.
 *
 * Parameters:
 *   ctp    : message context (notification value = dying process pid)
 *   code   : notification code (unused)
 *   flags  : dispatch flags (unused)
 *   handle : dispatch handle (unused)
 * Return value: 0.
 */
int proc_coredump(message_context_t *ctp,
                  int code,
                  unsigned flags,
                  void *handle)
{
    union sigval value = ctp->msg->notification.value;
    int dumped = 0;
    int fd;
    process_t *prp;

    if (proc_service_scoid != ctp->msg->notification.scoid) {
        return 0;
    }

    if (proc_thread_pool_reserve() == 0) {
        /* Try to cause dumper to write a core file */
        if ((fd = _connect(0, "/proc/dumper", 0, O_WRONLY, 0, _IO_CONNECT_OPEN,
                           0, _IO_FLAG_RD | _IO_FLAG_WR, _FTYPE_DUMPER, 0, 0, 0,
                           0, 0, 0)) != -1) {
            char buff[2 * (INT_STRLEN_MAXIMUM(int) + 1)];
            char *p;

            snprintf(buff, sizeof(buff), "%d", value.sival_int);
            p = &buff[strlen(buff)];
            if ((prp = root_proc_lock(value.sival_int))) {
                root_proc_info_t qinfo;
                if (process_query_info(value.sival_int, &qinfo) == EOK &&
                    qinfo.rlimit_vals_soft[RLIMIT_CORE] != RLIM_INFINITY) {
                    /* set a maximum core size. */
                    snprintf(p, (size_t) (&buff[sizeof(buff)] - p), " %llu",
                             (unsigned long long)
                                 qinfo.rlimit_vals_soft[RLIMIT_CORE]);
                    p = &buff[strlen(buff)];
                }
                root_proc_unlock(value.sival_int);
            }
            if (write(fd, buff, (size_t) (p - buff)) != -1) {
                /* A Dump file are written.... */
                dumped = 1;
            }
            close(fd);
        }

        proc_thread_pool_reserve_done();
    }

    /* Only leave the RTMUX_FLG_PROC_COREDUMP flag on if a dump file was really
     * created. The termer thread looks at this flag to report if a coredump
     * file was created.
     */
    if (!dumped) {
        if ((prp = root_proc_lock(value.sival_int))) {
            (void) ProcessFlagsOp((pid_t) value.sival_int, 0,
                                  RTMUX_FLG_PROC_COREDUMP);
            root_proc_unlock(value.sival_int);
        }
    }

    /* Let the process terminate normally... */
    (void) process_destroy_all(value.sival_int);

    return 0;
}
