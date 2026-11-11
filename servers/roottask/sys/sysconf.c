/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <confname.h>
#include <sys/conf.h>
#include "root-process.h"

static int roottask_sysconf_get(int name, long *value, char *str);
static int roottask_confstr_get(int name, long *value, char *str);

/* sc_nprocessors / sc_clk_tck — backing functions for _SC_NPROCESSORS_* and
 * _SC_CLK_TCK lookups. roottask pulls both from the syspage directly. The
 * syspage is read-only-mapped into every process, so this is safe from U-mode
 * roottask. Tick rate is derived from qtime.nsec_inc (nanoseconds per tick),
 * the syspage equivalent of clk_tck.
 */
static unsigned sc_nprocessors(int name)
{
    (void) name;
    return _syspage_ptr->num_cpu;
}

static unsigned sc_clk_tck(int name)
{
    (void) name;
    unsigned long inc = SYSPAGE_ENTRY(qtime)->nsec_inc;
    return inc ? (unsigned) (1000000000UL / inc) : 100;
}

/* roottask_max_fds is a 32-bit unsigned; a _CONF_INDIRECT entry loads a whole
 * long from the address and returned the next 4 bytes of .data in the upper
 * half. Widen it through a function instead.
 */
static long sc_open_max(int name)
{
    (void) name;
    return (long) roottask_max_fds;
}

static const long roottask_sysconf_table[] = {
    _CONF_CALL_FCN, (intptr_t) roottask_sysconf_get,
    _CONF_VALUE_NUM | _SC_ARG_MAX, ARG_MAX, _CONF_VALUE_NUM | _SC_VERSION,
    _POSIX_VERSION, _CONF_VALUE_MIN | _CONF_INDIRECT | _SC_PAGESIZE,
    (intptr_t) &root_vm_ops.pagesize,

    /* These entries live entirely on the roottask side; kernel scalars
     * (kopt.max_fds, clk_tck, num_processors) are not reachable from U-mode
     * roottask, so these come from roottask/syspage values.
     */
    _CONF_VALUE_NUM | _CONF_FCN | _SC_CLK_TCK, (intptr_t) sc_clk_tck,
    _CONF_VALUE_MIN | _CONF_FCN | _SC_OPEN_MAX, (intptr_t) sc_open_max,
    _CONF_VALUE_MIN | _SC_NGROUPS_MAX,
    NGROUPS_MAX, _CONF_VALUE_NUM | _SC_SAVED_IDS, 1,
    _CONF_VALUE_NUM | _SC_NZERO, 20, _CONF_VALUE_NUM | _SC_RTSIG_MAX,
    (SIGRTMAX - SIGRTMIN) + 1,

    /* PID_MASK and VECTOR_MAX live in kernel/core/macros.h (kernel-internal).
     * Hardcoded here with the same values to avoid pulling macros.h into
     * roottask. Stay in lockstep if kernel ever changes them.
     */
    _CONF_VALUE_MIN | _SC_CHILD_MAX, 0xfff - 2, /* PID_MASK - 2 */
    _CONF_VALUE_MAX | _SC_THREAD_STACK_MIN, PTHREAD_STACK_MIN,
    _CONF_VALUE_NUM | _SC_THREAD_THREADS_MAX, 0xffff - 1, /* VECTOR_MAX */
    _CONF_VALUE_NUM | _CONF_FCN | _SC_NPROCESSORS_CONF,
    (intptr_t) sc_nprocessors,
    _CONF_VALUE_NUM | _CONF_FCN | _SC_NPROCESSORS_ONLN,
    (intptr_t) sc_nprocessors, _CONF_VALUE_NUM | _SC_DELAYTIMER_MAX,

    /* aio is unsupported (no aio.h / aio_*); its _SC_AIO_* limits stay unset.
     */
    DELAYTIMER_MAX,

    _CONF_STR | _CONF_CALL_FCN, (intptr_t) roottask_confstr_get, _CONF_END};

struct conf_entry {
    struct conf_entry *next;
    struct conf_entry **prev;
    pid_t pid;
    long *list;
};

static struct conf_entry *conf_list;

static pthread_mutex_t conf_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Tear down a process's sysconf/confstr override table.
 *
 * Unlinks the process's conf_entry from the global list and frees both the
 * entry and its value list. Called when a process exits. Parameters:
 *   pid : the process whose conf table to destroy
 */
void roottask_sysconf_destroy(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    struct conf_entry *p;

    if ((p = tmp->conf_table)) {
        pthread_mutex_lock(&conf_mutex);
        if ((*p->prev = p->next)) {
            p->next->prev = p->prev;
        }
        pthread_mutex_unlock(&conf_mutex);
        if (p->list) {
            _conf_destroy(p->list);
        }
        _sfree(p, sizeof *p);
        tmp->conf_table = 0;
    }
}

/* Look up a per-process sysconf/confstr override value.
 *
 * Scans every process's override table for a matching name. Parameters:
 *   name  : the _SC_/_CS_ name to look up
 *   value : in/out — buffer size on input, value on output
 *   str   : output buffer for string (_CS_) values, or NULL
 * Return value: the owning pid on a match, -1 if not found.
 */
static int roottask_sysconf_get(int name, long *value, char *str)
{
    struct conf_entry *p;
    int ret = -1;

    if (pthread_mutex_lock(&conf_mutex) == EOK) {
        for (p = conf_list; p; p = p->next) {
            if (p->list) {
                if (_conf_get(p->list, name, value, str) != -1) {
                    ret = p->pid;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&conf_mutex);
    }
    return ret;
}

/* Look up a confstr (_CS_) value from the syspage typed-strings table.
 *
 * For names below _CS_VENDOR, spaces in the value are translated to
 * underscores. Parameters:
 *   name  : the _CS_ name to look up
 *   value : in/out — buffer size on input, copied length on output
 *   str   : output buffer for the string value
 * Return value: 0 on a match, -1 if not found.
 */
static int roottask_confstr_get(int name, long *value, char *str)
{
    struct entry {
        uint32_t type;
        char info[1];
    } *ent;

    for (ent = (struct entry *) SYSPAGE_ENTRY(typed_strings)->data;
         ent->type != _CS_NONE;
         ent = (struct entry *) ((char *) ent + ((offsetof(struct entry, info) +
                                                  strlen(ent->info) + 1 + 3) &
                                                 ~3UL))) {
        if (ent->type == name) {
            int size;
            char *p = ent->info;

            for (size = 0; size < *value && (str[size] = *p); p++, size++) {
                if ((*p == ' ') && (name < _CS_VENDOR)) {
                    str[size] = '_';
                }
            }
            while (*(p++)) {
                size++;
            }
            *value = ++size;
            return 0;
        }
    }
    return -1;
}

/* Set a sysconf/confstr override for a process (or the system).
 *
 * Allocates the process's override table on first use and stores the value,
 * then triggers a PROC_EVENT_SYSCONF/CONFSTR notification. Parameters:
 *   pid   : target process, or 0 for the system (ROOTTASK_PID) table
 *   cmd   : conf command/flags
 *   name  : the _SC_/_CS_ name being set
 *   value : numeric value to set
 *   str   : string value to set, or NULL for a numeric set
 * Return value: EOK on success, or an errno (EL2HLT, EBUSY, ENOMEM).
 */
int roottask_sysconf_set(pid_t pid,
                         unsigned cmd,
                         int name,
                         long value,
                         const char *str)
{
    process_t *prp;
    struct conf_entry *table;
    pid_t locked_pid = pid ? pid : ROOTTASK_PID;
    int status;

    if (!(prp = root_proc_lock(locked_pid))) {
        return EL2HLT;
    }

    if (pid) {
        if (str) {
            char buff[1];
            long tmp = sizeof buff;

            pid = _conf_get(roottask_sysconf_table, name, &tmp, buff);
        } else {
            long tmp = -1;

            pid = _conf_get(roottask_sysconf_table, name, &tmp, 0);
        }

        if (pid != -1 && pid != 0 && pid != locked_pid) {
            return proc_error(EBUSY, locked_pid);
        }
    }

    {
        root_process_t *tmp = root_proc(locked_pid);
        if (!(table = tmp->conf_table)) {
            if (!(table = tmp->conf_table = _scalloc(sizeof *table))) {
                return proc_error(ENOMEM, locked_pid);
            }
            table->pid = locked_pid;
            table->prev = &conf_list;
            pthread_mutex_lock(&conf_mutex);
            if ((table->next = conf_list)) {
                conf_list->prev = &table->next;
            }
            conf_list = table;
            pthread_mutex_unlock(&conf_mutex);
        }
    }

    status = _conf_set(&table->list, (int) cmd, name, value, str);
    if (status == EOK) {
        proc_event_trigger(str ? PROC_EVENT_CONFSTR : PROC_EVENT_SYSCONF);
    }
    return proc_error(status, locked_pid);
}

/* Handle a _SYS_CONF message: get a sysconf/confstr value.
 *
 * _SYS_SUB_GET looks it up and replies with the result. Performs endian
 * conversion of the message when the sender differs. Parameters:
 *   ctp : the message context
 *   msg : the sysconf message
 * Return value: an I/O service reply code, or an errno (EPERM, EINVAL,
 *               ENOSYS).
 */
int roottask_sysconf_msg(ioserv_context_t *ctp, sys_conf_t *msg)
{
    long value;

    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
        ENDIAN_SWAP16(&msg->i.subtype);
        ENDIAN_SWAP32(&msg->i.cmd);
        ENDIAN_SWAP32(&msg->i.name);
        ENDIAN_SWAP64(&msg->i.value);
    }
    switch (msg->i.subtype) {
    case _SYS_SUB_GET:
        if (msg->i.cmd & _CONF_STR) {
            long room = (long) (ctp->msg_max_size - sizeof msg->i);
            size_t size;

            size = (size_t) msg->i.value;
            value = min_long(msg->i.value, room);
            msg->o.match = _conf_get(roottask_sysconf_table, msg->i.name,
                                     &value, (char *) (&msg->o + 1));
            msg->o.value = min_long(value, room);
            if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
                ENDIAN_SWAP32(&msg->o.match);
                ENDIAN_SWAP64(&msg->o.value);
            }
            return IOSERV_PTR(
                ctp, &msg->o,
                sizeof msg->o + min_size(size, (size_t) msg->o.value));
        } else if (msg->i.cmd & _CONF_NUM) {
            value = msg->i.value;
            msg->o.match =
                _conf_get(roottask_sysconf_table, msg->i.name, &value, 0);
            msg->o.value = value;
            if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
                ENDIAN_SWAP32(&msg->o.match);
                ENDIAN_SWAP64(&msg->o.value);
            }
            return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
        }
        return EINVAL;

    default:
        break;
    }
    return ENOSYS;
}
