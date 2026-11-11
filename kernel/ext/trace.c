/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <unistd.h>

struct kcall_args_trace_args {
    int ev_class;
    int ev_code;
    int iov_cnt;
    iov_t *iovs;
};

#if defined(CONFIG_INSTRUMENTATION)

#define _TRACE_CKH_EXE_EHB(e1, e2, a1, a2, a3)         \
    ((!(e1) || exe_event_h_buff(1, e1, a1, a2, a3)) && \
     (!(e2) || exe_event_h_buff(1, e2, a1, a2, a3)))

/* Decide whether a proc-service trace event may be emitted.
 *
 * Acts as the generic filter point for proc-service events: events that the
 * trace masks are not configured for are rejected. In non-wide mode it may also
 * reduce the iov count so less data is emitted. The hard-coded filtering here
 * is the migration path toward a fully generic filter in nano_trace.
 * Parameters:
 *   header           : trace header encoding event class and code
 *   iov              : iovec describing the event's first data element
 *   iovadjustedcount : in/out iov count, possibly capped for non-wide mode
 * Return value: non-zero if the event may be emitted, 0 otherwise.
 */
static int can_emit_event(uint32_t header, iov_t *iov, int *iovadjustedcount)
{
    int event_class = RTMUX_TRACE_GETEVENT_C(header);
    int event_code = RTMUX_TRACE_GETEVENT(header);

    switch (event_class) {
    case _TRACE_SYSTEM_C:
        switch (event_code) {
        case RTMUX_TRACE_SYS_PATH:
        case RTMUX_TRACE_SYS_MMAP:
        case RTMUX_TRACE_SYS_MUNMAP:
        case RTMUX_TRACE_SYS_MAPNAME:
        case RTMUX_TRACE_SYS_SLOG:
            if (!(trace_masks.system_mask[event_code] & _TRACE_ENTER_SYSTEM)) {
                return 0;
            }
            /* Non-wide mode may require a reduction in data emitted */
            if (!(trace_masks.system_mask[event_code] &
                  _TRACE_SYSTEM_ARG_WIDE)) {
                switch (event_code) {
                case RTMUX_TRACE_SYS_MMAP: /* Depend */
                    if ((*iovadjustedcount) > 3) {
                        *iovadjustedcount = 3;
                    }
                    break;
                default:
                    break;
                }
            }
            /* We only check the first bit of the data */
            return _TRACE_CKH_EXE_EHB(trace_masks.class_system_ehd_p,
                                      trace_masks.system_ehd_p[event_code],
                                      header, GETIOVBASE(iov),
                                      (uint32_t) GETIOVLEN(iov));

        default:
            break;
        }
        break;

    default:
        break;
    }

    return 0; /* The default is not to emit any event */
}


/* Emit a trace event into the kernel trace ring (PRIV kernel extension body).
 *
 * Builds the trace header for the calling CPU, and if can_emit_event passes the
 * filter, appends the iovecs to the trace ring. trace_slock guards the
 * (currently single, coarse) ring write; per-CPU rings remain a future
 * refinement. Parameters:
 *   data : pointer to a struct kcall_args_trace_args (event class/code + iovs)
 */
void kext_trace(void *data)
{
    struct kcall_args_trace_args *kap = data;

    if (kap->iov_cnt > 0) {
        uint32_t header;
        int iovcnt;

        iovcnt = kap->iov_cnt;
        header = _TRACE_MAKE_CODE(get_cpunum(), _TRACE_STRUCT_S, kap->ev_class,
                                  kap->ev_code);

        kspinlock_lock(&trace_slock);
        if (can_emit_event(header, kap->iovs, &iovcnt)) {
            add_trace_iovs(header, kap->iovs, (unsigned) iovcnt);
        }
        kspinlock_unlock(&trace_slock);
    }

    SETKSTATUS(get_active_thread(get_cpunum()), 0);
}

/* Add a trace event described by an iovec array.
 *
 * Packs the event into kcall_args and dispatches the __KSYS_PRIV_TRACE kernel
 * extension. Parameters:
 *   ev_class : trace event class
 *   ev_code  : trace event code
 *   iovs     : iovec array describing the event data
 *   iovcnt   : number of iovecs
 * Return value: 0 on success, -errno on failure.
 */
int kext_add_trace_event_iov(int ev_class, int ev_code, iov_t *iovs, int iovcnt)
{
    struct kcall_args_trace_args args;

    args.ev_class = ev_class;
    args.ev_code = ev_code;
    args.iov_cnt = iovcnt;
    args.iovs = iovs;

    return (int) syscall_priv(__KSYS_PRIV_TRACE, (void *) &args);
}

/* Add a trace event described by a single contiguous buffer.
 *
 * Wraps the buffer in a one-element iovec and forwards to
 * kext_add_trace_event_iov. Parameters:
 *   ev_class : trace event class
 *   ev_code  : trace event code
 *   data     : pointer to the event data
 *   datalen  : length of the event data in bytes
 * Return value: 0 on success, -errno on failure.
 */
int kext_add_trace_event(int ev_class, int ev_code, void *data, int datalen)
{
    iov_t iov[1];

    SETIOV(iov, data, datalen);

    return kext_add_trace_event_iov(ev_class, ev_code, iov, 1);
}

#else

/* Add a trace event described by an iovec array (instrumentation disabled).
 *
 * No-op stub built when CONFIG_INSTRUMENTATION is off. Parameters:
 *   ev_class : trace event class (ignored)
 *   ev_code  : trace event code (ignored)
 *   iovs     : iovec array (ignored)
 *   iovlen   : number of iovecs (ignored)
 * Return value: always 0.
 */
int kext_add_trace_event_iov(int ev_class, int ev_code, iov_t *iovs, int iovlen)
{
    return 0;
}

/* Add a trace event from a single buffer (instrumentation disabled).
 *
 * No-op stub built when CONFIG_INSTRUMENTATION is off. Parameters:
 *   ev_class : trace event class (ignored)
 *   ev_code  : trace event code (ignored)
 *   data     : event data (ignored)
 *   datalen  : event data length (ignored)
 * Return value: always 0.
 */
int kext_add_trace_event(int ev_class, int ev_code, void *data, int datalen)
{
    return 0;
}

#endif
