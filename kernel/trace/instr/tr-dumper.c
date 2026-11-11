/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * tr-dumper.c — kernel-side dumper for the tracelogger ring buffer.
 *
 * Called from the S-mode trap handler right before the CPU halts on an
 * unrecoverable exception, so the console log of a crash automatically ends
 * with the last ~hundred trace events. No user-space tracelogger daemon is
 * needed to see the sequence of kernel activity that led up to the fault.
 *
 * The dump walks the ring of trace buffers starting at trace_masks.buff_0_ptr.
 * Each event is 4 x uint32_t: header[31:30] = struct (S/CB/CC/CE),
 * header[29:24] = CPU, header[13:10] = class, header[9:0] = event code within
 * class.
 *
 * If CONFIG_INSTRUMENTATION is not enabled, or no buffer was ever allocated (no
 * tracelogger started), tr_dump_to_console() prints a one-line "not available"
 * message and returns.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

#if defined(CONFIG_INSTRUMENTATION)

#include <trace/events.h>
#include <sys/trace.h>

static const char *class_name(unsigned cl)
{
    switch (cl) {
    case 0:
        return "EMPTY  ";
    case 1:
        return "CONTROL";
    case 2:
        return "KCALL";
    case 3:
        return "INT    ";
    case 4:
        return "PR_TH  ";
    case 5:
        return "SYSTEM ";
    case 6:
        return "USER   ";
    case 7:
        return "COMM   ";
    default:
        return "???    ";
    }
}

static const char *struct_name(unsigned s)
{
    switch (s) {
    case 0:
        return "S "; /* simple          */
    case 1:
        return "CB"; /* combined begin  */
    case 2:
        return "CC"; /* combined cont.  */
    case 3:
        return "CE"; /* combined end    */
    default:
        return "??";
    }
}

/* Cap the per-buffer dump to the most-recent TR_DUMP_TAIL events. The crash
 * path's per-CPU buffer is 4 KiB; each trace line is ~80 bytes, so ~50 events ×
 * 4 buffers fits inside. Without the cap the dump prints from the oldest event
 * forward and silently truncates once the buffer fills — exactly when we lose
 * the events that actually caused the crash.
 */
#define TR_DUMP_TAIL 50

static void dump_buffer(const tracebuf_t *b)
{
    uint32_t n = b->h.num_events;
    if (n == 0)
        return;
    if (n > _TRACELEMENTS)
        n = _TRACELEMENTS;

    /* Keep the tail: events [start..n-1] are the most recent. */
    uint32_t start = (n > TR_DUMP_TAIL) ? (n - TR_DUMP_TAIL) : 0;

    kprintf("--- buffer seq=%u num=%u tail=%u..%u ---\n",
            (unsigned) b->h.seq_buff_num, (unsigned) n, (unsigned) start,
            (unsigned) (n - 1));

    const traceevent_t *ev = (const traceevent_t *) b->h.begin_ptr;
    for (uint32_t i = start; i < n; i++) {
        uint32_t h = ev[i].header;
        unsigned st = (h >> 30) & 0x3;
        unsigned cp = (h >> 24) & 0x3f;
        unsigned cl = (h >> 10) & 0xf;
        unsigned ec = h & 0x3ff;
        kprintf("  [%4u] cpu%u %s %s ev=%3u  %08x %08x %08x\n", (unsigned) i,
                cp, struct_name(st), class_name(cl), ec,
                (unsigned) ev[i].data[0], (unsigned) ev[i].data[1],
                (unsigned) ev[i].data[2]);
    }
}

/* tr_dump_to_console — print every non-empty trace buffer to the kernel
 * console. Safe to call from a crashing trap handler: only reads trace_masks
 * fields, does not take any spin lock. tr_autostart — allocate trace buffers
 * from the kernel heap and arm tracing. Called from kernel_init() when
 * kopt.auto_trace is set by the '-T' command-line option. Equivalent to a
 * user-space tracelogger issuing RTMUX_TRACE_ALLOCBUFFER +
 * RTMUX_TRACE_STARTNOSTATE before any other syscall, except the buffer is a
 * kernel-heap allocation and nothing ever reads it out (the only consumer is
 * the crash-path tr_dump_to_console()).
 */
#define TR_AUTOSTART_N_BUFS 4 /* 4 * 16 KiB = 64 KiB */

void tr_autostart(void)
{
    const unsigned n = TR_AUTOSTART_N_BUFS;
    tracebuf_t *bufs;
    unsigned i;

    if (trace_masks.buff_0_ptr)
        return; /* someone already allocated */

    bufs = (tracebuf_t *) _smalloc(n * sizeof(tracebuf_t));
    if (!bufs) {
        kprintf("tr_autostart: _smalloc(%lu) failed\n",
                (unsigned long) (n * sizeof(tracebuf_t)));
        return;
    }

    for (i = 0; i < n; i++) {
        bufs[i].h.flags = 0;
        bufs[i].h.num_events = 0;
        bufs[i].h.ls13_rb_num = i << 13;
        bufs[i].h.seq_buff_num = 0;
        bufs[i].h.begin_ptr = (traceevent_t *) &bufs[i].data[0];
        bufs[i].h.tail_ptr = bufs[i].h.begin_ptr;
        bufs[i].h.baseaddr = bufs;
        bufs[i].h.next = &bufs[i + 1];
        memset(&bufs[i].h.spin, 0, sizeof(bufs[i].h.spin));
    }
    bufs[n - 1].h.next = &bufs[0]; /* close the ring */

    trace_masks.buff_0_ptr = bufs;
    trace_masks.buff_num = n;
    trace_masks.max_events = _TRACELEMENTS - _TRACEBUFEMPTY;
    trace_masks.ring_mode = 1; /* wrap oldest events */
    trace_masks.skip_buff = 0;
    privateptr->tracebuf = bufs;

    /* Enable every class mask so we capture everything of interest. This is
     * blunt — a real tracelogger would set selective masks — but for crash-dump
     * purposes we want as much context as possible.
     */
    trace_masks.main_flags = 1;
    {
        unsigned k;
        for (k = 0; k < _TRACE_MAX_KER_CALL_NUM; k++)
            trace_masks.ksys_call_masks[k] =
                _TRACE_ENTER_CALL | _TRACE_EXIT_CALL;
        for (k = 0; k < _TRACE_MAX_COMM_NUM; k++)
            trace_masks.comm_mask[k] = _TRACE_ENTER_COMM;
        for (k = 0; k < _TRACE_MAX_TH_STATE_NUM; k++) {
            trace_masks.thread_pid[k] = 0;
            trace_masks.thread_tid[k] = 0;
        }
    }

    kprintf("tr_autostart: %u buffers x %u events armed at %p\n", n,
            (unsigned) _TRACELEMENTS, (void *) bufs);
}

void tr_dump_to_console(void)
{
    tracebuf_t *b0 = (tracebuf_t *) trace_masks.buff_0_ptr;
    uint32_t n = trace_masks.buff_num;

    if (!b0 || n == 0) {
        kprintf("*** trace buffer not allocated — nothing to dump ***\n");
        return;
    }

    kprintf("\n*** trace buffer dump (%u buffers, %u events each max) ***\n",
            (unsigned) n, (unsigned) _TRACELEMENTS);

    tracebuf_t *b = b0;
    for (uint32_t i = 0; i < n; i++) {
        dump_buffer(b);
        b = b->h.next;
        if (!b || b == b0)
            break;
    }
    kprintf("*** trace buffer dump end ***\n");
}

#else /* !CONFIG_INSTRUMENTATION */

void tr_autostart(void)
{
    /* Tracing not compiled in — nothing to do. */
}

void tr_dump_to_console(void)
{
    /* Tracing not compiled in — nothing to do. */
}

#endif
