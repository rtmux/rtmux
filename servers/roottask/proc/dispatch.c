/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/states.h>
#include "externs.h"
#include "private.h"
#include "root-process.h"

/* proc_message_handler — dispatch an incoming _PROC_* / _SYS_* message to the
 * matching handler.
 *
 * Decodes the message type (byte-swapping the header first on a cross-endian
 * client) and routes to the appropriate process or roottask service routine.
 *
 * Parameters:
 *   mctp   : message context (cast to a I/O service context)
 *   code   : message notification code (unused)
 *   flags  : dispatch flags (unused)
 *   handle : dispatch handle (unused)
 * Return value: I/O service status from proc_status().
 */
static int proc_message_handler(message_context_t *mctp,
                                int code,
                                unsigned flags,
                                void *handle)
{
    int n = ENOSYS;
    ioserv_context_t *ctp = (ioserv_context_t *) mctp;
    union proc_msg_union_local {
        uint16_t type;
        proc_getsetid_t getsetid;
        proc_setpgid_t setpgid;
        proc_wait_t wait;
        proc_umask_t umask;
        proc_session_t session;
        proc_event_t event;
        proc_syschan_t syschan;
        proc_detach_t detach;
        proc_phys_contain_t phys_contain;
        proc_kill_t kill;
        proc_jobctl_t jobctl;
        sys_conf_t conf;
        sys_cmd_t command;
    } *msg = (union proc_msg_union_local *) (void *) ctp->msg;

    ctp->status = 0;
    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
        ENDIAN_SWAP16(&msg->type);
    }
    switch (msg->type) {
    case _PROC_GETSETID:
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            n = ENOTSUP;
        } else {
            n = proc_getsetid(ctp, &msg->getsetid);
        }
        break;

    case _PROC_SETPGID:
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            n = ENOTSUP;
        } else {
            n = proc_setpgid(ctp, &msg->setpgid);
        }
        break;

    case _PROC_WAIT:
        n = proc_wait(ctp, &msg->wait);
        break;

    case _PROC_SPAWN:
        n = proc_spawn(ctp, msg);
        break;

    case _PROC_POSIX_SPAWN:
        n = proc_pspawn(ctp, msg);
        break;

    case _PROC_UMASK:
        n = proc_umask(ctp, &msg->umask);
        break;

    case _PROC_SESSION:
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            n = ENOTSUP;
        } else {
            n = proc_msg_session(ctp, &msg->session);
        }
        break;

    case _PROC_EVENT:
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            n = ENOTSUP;
        } else {
            n = proc_event(ctp, &msg->event);
        }
        break;

    case _PROC_RESOURCE:
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            n = ENOTSUP;
        } else {
            n = proc_msg_resource(ctp, msg);
        }
        break;

    case _PROC_SYSCHAN:
        n = proc_syschan(ctp, &msg->syschan);
        break;

    case _PROC_DETACH:
        n = proc_detach(ctp, &msg->detach);
        break;

    case _PROC_PHYS_CONTAIN:
        n = proc_phys_contain(ctp, &msg->phys_contain);
        break;

    case _PROC_KILL:
        n = proc_signal(ctp, &msg->kill);
        break;

    case _PROC_JOBCTL:
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
            n = ENOTSUP;
        } else {
            n = proc_msg_jobctl(ctp, &msg->jobctl);
        }
        break;

    case _PROC_EXIT:
        n = proc_exit(ctp, (union proc_msg_union *) (void *) msg);
        break;

    case _SYS_CONF:
        n = roottask_sysconf_msg(ctp, &msg->conf);
        break;

    case _SYS_CMD:
        n = roottask_command(ctp, &msg->command);
        break;

    case _SYS_CONSOLE:
        n = roottask_console_msg(ctp, (sys_console_t *) (void *) msg);
        break;

    default:
        break;
    }

    return proc_status(ctp, n);
}

/* proc_dispatch_init — wire up the process service.
 *
 * Attaches the process notifications, imports the stack notification code into
 * the kernel, then routes _SYS and _PROC messages to proc_message_handler.
 */
void proc_dispatch_init(void)
{
    struct _server_info info;

    notification_attach(dpp, 0, PROC_CODE_TERM, proc_terminate, NULL);
    notification_attach(dpp, 0, PROC_CODE_STOP, proc_stop_or_continue, NULL);
    notification_attach(dpp, 0, PROC_CODE_CONT, proc_stop_or_continue, NULL);

    (void) notification_attach(dpp, MSG_FLAG_ALLOC_NOTIFICATION, 0,
                               proc_coredump, NULL);
    (void) connect_server_info(ROOTTASK_PID, ROOTTASK_COID, &info);
    proc_service_scoid = info.scoid;

    proc_hooks_t process_reg = {
        .proc_stack_code = notification_attach(dpp, MSG_FLAG_ALLOC_NOTIFICATION,
                                               0, proc_stack, 0),
    };
    {
        syscall_priv(__KSYS_PRIV_REGISTER_PROC_HOOKS, &process_reg);
    }

    /* Redirect _PROC_* messages to proc handler */
    message_attach(dpp, NULL, _SYS_BASE, _SYS_MAX, proc_message_handler, NULL);
    message_attach(dpp, NULL, _PROC_BASE, _PROC_MAX, proc_message_handler,
                   NULL);
}
