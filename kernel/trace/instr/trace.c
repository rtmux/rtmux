/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#if defined(CONFIG_INSTRUMENTATION)
#include <trace/events.h>
#include <user-slice.h>
#include "pa.h"

#define PAA_FLAG_CONTIG 0x01

/* Ceiling on RTMUX_TRACE_ALLOCBUFFER's buffer count. 1024 * 16 KiB = 16 MiB of
 * physically contiguous RAM, which is already far past what pa_alloc will hand
 * out in practice (the in-tree tracers ask for 16) and keeps the header's
 * `i << 13` buffer number inside 32 bits with room to spare. See the
 * `_TRACE_MAX_BUFFERS` row in docs/assumptions.md.
 */
#define _TRACE_MAX_BUFFERS 1024

extern struct pa_quantum *pa_alloc(paddr_t size,
                                   paddr_t align,
                                   unsigned color,
                                   unsigned flags,
                                   unsigned *status_p,
                                   struct pa_restrict *restriction,
                                   paddr_t resv_size);
extern void pa_free(struct pa_quantum *paq, size_t num, paddr_t resv_size);
extern paddr_t pa_quantum_to_paddr(struct pa_quantum *pq);
extern struct pa_restrict *restrict_user;

static struct pa_quantum *tracebuf_pq;

/* Validate a trace class identifier.
 *
 * Parameters:
 *   class : trace class (RTMUX_TRACE_*)
 * Return value: EOK if the class is recognized, else ENOTSUP
 */
static int check_class(unsigned class)
{
    switch (class) {
    case RTMUX_TRACE_EMPTY:
    case RTMUX_TRACE_CONTROL:
    case RTMUX_TRACE_KCALL:
    case RTMUX_TRACE_KCALLENTER:
    case RTMUX_TRACE_KCALLEXIT:
    case RTMUX_TRACE_KCALLINT:
    case RTMUX_TRACE_INT:
    case RTMUX_TRACE_INTENTER:
    case RTMUX_TRACE_INTEXIT:
    case RTMUX_TRACE_INT_HANDLER_ENTER:
    case RTMUX_TRACE_INT_HANDLER_EXIT:
    case RTMUX_TRACE_PROCESS:
    case RTMUX_TRACE_THREAD:
    case RTMUX_TRACE_VTHREAD:
    case RTMUX_TRACE_SYSTEM:
    case RTMUX_TRACE_COMM: {
        return EOK;
    }
    default: {
        return ENOTSUP;
    }
    }
}

/* Resolve a trace interrupt-selector to a flat interrupt level.
 *
 * RTMUX_TRACE_INTFIRST maps to 0; RTMUX_TRACE_INTLAST maps to the total vector
 * count across all intrinfo entries; anything else is passed to
 * get_interrupt_level(). Parameters:
 *   v : interrupt selector
 * Return value: the interrupt level, or get_interrupt_level()'s result
 */
static int get_int_level(unsigned v)
{
    switch (v) {
    case RTMUX_TRACE_INTFIRST:
        return 0;
    case RTMUX_TRACE_INTLAST: {
        unsigned i;
        unsigned l = 0;
        struct intrinfo_entry *iip = intrinfoptr;

        for (i = 0; i < intrinfo_num; ++i, ++iip) {
            l += iip->num_vectors;
        }

        return (int) l;
    }
    default:
        return get_interrupt_level(NULL, v);
    }
}

/* Register a trace event handler.
 *
 * Allocates a free slot in trace_masks.eh_storage, records the handler, its
 * data area, owning thread/process and register snapshot, and links the slot
 * back through *loc. Parameters:
 *   thp     : thread installing the handler
 *   loc     : location to store the resulting slot pointer
 *   handler : event handler function
 *   area    : event data area
 * Return value: EOK on success, ENOTSUP if no slots are available
 */
static int add_eh(thread_t *thp,
                  ehandler_data_t **loc,
                  void *handler,
                  event_data_t *area)
{
    interrupt_lock(&trace_masks.eh_spin);
    if (thp->process && trace_masks.eh_num < _TRACE_MAX_EV_HANDLER_NUM) {
        unsigned i;

        ++trace_masks.eh_num;
        for (i = 0;; ++i) {
            if (!trace_masks.eh_storage[i].handler) {
                trace_masks.eh_storage[i].area = area;
                trace_masks.eh_storage[i].thp = thp;
                trace_masks.eh_storage[i].process = thp->process;
                trace_masks.eh_storage[i].location = loc;
                trace_masks.eh_storage[i].handler =
                    (int (*)(event_data_t *)) handler;
                _TRACE_SAVE_REGS(thp, &trace_masks.eh_storage[i].cpu);
                *loc = trace_masks.eh_storage + i;
                interrupt_unlock(&trace_masks.eh_spin);

                return EOK;
            }
        }
    } else {
        interrupt_unlock(&trace_masks.eh_spin);

        return ENOTSUP;
    }
}

/* Remove a trace event handler previously installed by add_eh.
 *
 * Clears the slot pointed to by *loc and decrements the handler count.
 * Idempotent: a NULL *loc is ignored. Parameters:
 *   loc : location holding the slot pointer to release
 * Return value: EOK
 */
static int delete_eh(ehandler_data_t **loc)
{
    interrupt_lock(&trace_masks.eh_spin);
    if (*loc) {
        ehandler_data_t *eh_p = *loc;

        *loc = NULL;
        eh_p->handler = NULL;
        eh_p->area = NULL;
        eh_p->thp = NULL;
        eh_p->process = NULL;
        eh_p->location = NULL;
        --trace_masks.eh_num;
    }
    interrupt_unlock(&trace_masks.eh_spin);

    return EOK;
}

/* Local macros used only within kernel trace call. */
#define _TRACE_ADD_EH(loc)                                  \
    ((a_1) = (loc), (a_2) = (*(++c_p)), (a_3) = (*(++c_p)), \
     add_eh(act, a_1, TRACE_PACK_PTR(void *, a_2),          \
            TRACE_PACK_PTR(event_data_t *, a_3)))

#define _TRACE_DOUBLE_ADD_EH(l_1, l_2)                       \
    (a_0 = l_1, a_1 = l_2, a_2 = *(++c_p), a_3 = *(++c_p),   \
     ((add_eh(act, a_0, TRACE_PACK_PTR(void *, a_2),         \
              TRACE_PACK_PTR(event_data_t *, a_3)) == EOK && \
       add_eh(act, a_1, TRACE_PACK_PTR(void *, a_2),         \
              TRACE_PACK_PTR(event_data_t *, a_3)) == EOK)   \
          ? EOK                                              \
          : ENOTSUP))

/* Words the caller's command array must hold, keyed by opcode.
 *
 * Derived from the RTMUX_TRACE_* constants themselves, so the table cannot
 * drift from the ABI. Every opcode carries its word count in its top nibble,
 * but that nibble is caller-supplied data at runtime while the switch
 * dispatches on the low 28 bits: taking the length from the caller's nibble
 * would let a forged word pair a long opcode with a short copy, and the case
 * body would then read uninitialized kernel stack. Index by the low bits and
 * ignore the caller's nibble entirely. A zero entry is an opcode this kcall
 * does not accept.
 *
 * RTMUX_TRACE_ALLOCBUFFER is the one hand-corrected row: it reads a full
 * pointer out of words 2 and 3 and writes through it, and its declared count of
 * 3 predates LP64. The nibble is part of the opcode value, so it is ABI and
 * cannot be fixed at its definition.
 */
#define _TRACE_CMD_LEN(op) \
    [_TRACE_GET_FLAG(op)] = (uint8_t) _TRACE_GET_WORDS(op)

static const uint8_t trace_cmd_len[] = {
    _TRACE_CMD_LEN(RTMUX_TRACE_ADDALLCLASSES),
    _TRACE_CMD_LEN(RTMUX_TRACE_DELALLCLASSES),
    _TRACE_CMD_LEN(RTMUX_TRACE_ADDCLASS),
    _TRACE_CMD_LEN(RTMUX_TRACE_DELCLASS),
    _TRACE_CMD_LEN(RTMUX_TRACE_ADDEVENT),
    _TRACE_CMD_LEN(RTMUX_TRACE_DELEVENT),
    _TRACE_CMD_LEN(RTMUX_TRACE_ADDEVENTHANDLER),
    _TRACE_CMD_LEN(RTMUX_TRACE_DELEVENTHANDLER),
    _TRACE_CMD_LEN(RTMUX_TRACE_INSERTEVENT),
    _TRACE_CMD_LEN(RTMUX_TRACE_START),
    _TRACE_CMD_LEN(RTMUX_TRACE_STOP),
    [_TRACE_GET_FLAG(RTMUX_TRACE_ALLOCBUFFER)] = 4, /* LP64 pointer in 2..3 */
    _TRACE_CMD_LEN(RTMUX_TRACE_DEALLOCBUFFER),
    _TRACE_CMD_LEN(RTMUX_TRACE_FLUSHBUFFER),
    _TRACE_CMD_LEN(RTMUX_TRACE_QUERYEVENTS),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETALLCLASSESFAST),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETALLCLASSESWIDE),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETCLASSFAST),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETCLASSWIDE),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETEVENTFAST),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETEVENTWIDE),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETCLASSPID),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETCLASSTID),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETEVENTPID),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETEVENTTID),
    _TRACE_CMD_LEN(RTMUX_TRACE_CLRCLASSPID),
    _TRACE_CMD_LEN(RTMUX_TRACE_CLRCLASSTID),
    _TRACE_CMD_LEN(RTMUX_TRACE_CLREVENTPID),
    _TRACE_CMD_LEN(RTMUX_TRACE_CLREVENTTID),
    _TRACE_CMD_LEN(RTMUX_TRACE_INSERTSUSEREVENT),
    _TRACE_CMD_LEN(RTMUX_TRACE_INSERTCUSEREVENT),
    _TRACE_CMD_LEN(RTMUX_TRACE_INSERTUSRSTREVENT),
    _TRACE_CMD_LEN(RTMUX_TRACE_STARTNOSTATE),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETRINGMODE),
    _TRACE_CMD_LEN(RTMUX_TRACE_SETLINEARMODE),
    _TRACE_CMD_LEN(RTMUX_TRACE_ADDCLASSEVHANDLER),
    _TRACE_CMD_LEN(RTMUX_TRACE_DELCLASSEVHANDLER),
    _TRACE_CMD_LEN(RTMUX_TRACE_SKIPBUFFER),
    _TRACE_CMD_LEN(RTMUX_TRACE_OVERWRITEBUFFER),
    _TRACE_CMD_LEN(RTMUX_TRACE_QUERYVERSION),
    _TRACE_CMD_LEN(RTMUX_TRACE_QUERYSUPPORT),
    _TRACE_CMD_LEN(RTMUX_TRACE_INSERTSCLASSEVENT),
    _TRACE_CMD_LEN(RTMUX_TRACE_INSERTCCLASSEVENT),
};

#undef _TRACE_CMD_LEN

/* Words to copy for this opcode, or 0 if the opcode is unknown or needs more
 * than the caller's buffer holds. Parameters:
 *   opcode : first word of the caller's command array
 *   max    : words the destination buffer can hold
 */
static unsigned trace_cmd_words(uint32_t opcode, unsigned max)
{
    unsigned flag = _TRACE_GET_FLAG(opcode);
    unsigned words;

    if (flag >= NUM_ELTS(trace_cmd_len))
        return 0;
    words = trace_cmd_len[flag];
    if (words == 0 || words > max)
        return 0;

    return words;
}

/* Copy a caller word payload named by the pointer pair at ptr[0..1] into
 * `out`, then point that pair at `out`. A zero count names no payload and
 * leaves the pair alone (add_trace_buffer reads nothing for it). Parameters:
 *   ptr    : the pointer-word pair in the kernel copy of the command
 *   nwords : caller's word count, at most TRACE_USRSTR_MAX / 4
 *   out    : kernel buffer of at least nwords + 2 words
 * Return value: EOK, EINVAL for an oversize count, EFAULT for a bad payload.
 */
static int trace_copy_words(thread_t *act,
                            uint32_t *ptr,
                            uint32_t nwords,
                            uint32_t *out)
{
    int rc;

    if (nwords > TRACE_USRSTR_MAX / sizeof(uint32_t))
        return EINVAL;
    if (!nwords)
        return EOK;
    rc = trace_read_user(act, TRACE_UNPACK_PTR(const void *, ptr[0], ptr[1]),
                         out, nwords * sizeof(uint32_t));
    if (rc != EOK)
        return rc;
    ptr[0] = TRACE_PTR_LO(out);
    ptr[1] = TRACE_PTR_HI(out);

    return EOK;
}

int kdecl ksys_trace_event(thread_t *act, struct kcall_args_trace_event *kap)
{
    unsigned *c_p;

    /* Zeroed: a case body reading past `words` sees 0, not stack junk.
     * aligned(8) because RTMUX_TRACE_ALLOCBUFFER reads a pointer out of words 2
     * and 3: uint32_t[] only guarantees 4, and the trap handler recovers page
     * faults but not RISCV_EXC_LOAD_MISALIGNED.
     */
    uint32_t cmd[8] __attribute__((aligned(8))) = {0};
    unsigned words;
    ehandler_data_t **a_0;
    ehandler_data_t **a_1;
    unsigned a_2;
    unsigned a_3;
    int rc;
    uint32_t payload[TRACE_USRSTR_MAX / sizeof(uint32_t) + 2] = {0};

    /* Read the command array into kernel memory before doing anything with it.
     * trace_read_cmd range-checks and copies fault-safely, so a null,
     * out-of-range, or unmapped array returns EFAULT instead of faulting the
     * kernel. The switch then dispatches on the kernel copy.
     *
     * That covers the command words, not the buffers they name. The three
     * opcodes that name a caller buffer (INSERTCUSEREVENT, INSERTUSRSTREVENT,
     * INSERTCCLASSEVENT) copy it into the kernel payload buffer below, again
     * before any lock is taken, and trace_event() only ever sees that copy.
     *
     * The opcode word is read exactly once and never re-read: the tail copy
     * starts at word 1, so the length trace_cmd_words derived and the opcode
     * the switch dispatches on are the same value. Re-reading it would let a
     * second thread in the caller's process swap a short opcode for a long one
     * between the two reads and leave the case body reading uninitialized cmd[]
     * tail.
     *
     * The former code probed exactly one word with a bare load, never
     * range-checked kap->data at all, and then read c_p[1..4] for the
     * multi-word commands. Since this kcall arms no xfer fault handler and
     * S-mode runs with SSTATUS.SUM set, that made trace_event(NULL) an
     * unprivileged kernel crash and an in-range kernel pointer a kernel read.
     */
    if (trace_read_cmd(act, kap->data, cmd, 1) != EOK)
        return EFAULT;
    words = trace_cmd_words(cmd[0], (unsigned) NUM_ELTS(cmd));
    if (!words) {
        /* Unknown opcode. ENOTSUP, not EINVAL: this used to fall through to the
         * switch default and out through trace_event(), and the stub variant
         * still answers ENOTSUP for the same input. The two builds must agree.
         */
        return ENOTSUP;
    }
    if (words > 1 &&
        trace_read_cmd(act, kap->data + 1, cmd + 1, words - 1) != EOK)
        return EFAULT;

    c_p = cmd;
    /* Caller payloads are copied here, with no lock held, and the command's
     * pointer words are overwritten with the kernel copy's address, so the
     * trace_event() arms never touch user memory and cannot fault under a
     * spinlock or race a concurrent munmap. The payload is read exactly once.
     * Fixed-size and zero-filled: add_trace_buffer reads whole event pairs, so
     * an odd word count reads one word past `nwords`.
     */
    switch (_TRACE_GET_FLAG(cmd[0])) {
    case _TRACE_GET_FLAG(RTMUX_TRACE_INSERTCUSEREVENT):
        rc = trace_copy_words(act, &cmd[2], cmd[4], payload);
        if (rc != EOK)
            return rc;
        break;
    case _TRACE_GET_FLAG(RTMUX_TRACE_INSERTCCLASSEVENT):
        rc = trace_copy_words(act, &cmd[3], cmd[5], payload);
        if (rc != EOK)
            return rc;
        break;
    case _TRACE_GET_FLAG(RTMUX_TRACE_INSERTUSRSTREVENT): {
        /* The terminator must lie within TRACE_USRSTR_MAX bytes. */
        const char *s = TRACE_UNPACK_PTR(const char *, cmd[2], cmd[3]);

        rc = trace_read_str(act, s, (char *) payload, TRACE_USRSTR_MAX);
        if (rc != EOK)
            return rc;
        cmd[2] = TRACE_PTR_LO(payload);
        cmd[3] = TRACE_PTR_HI(payload);
        break;
    }
    default:;
    }

    /* FIXME: for many of the cases we're locking far earlier than we need to.
     * This makes it clear in each case that we are locked, but at the cost of
     * locking earlier than we really need to. We may want to refrain from
     * locking here and place a lock at the appropriate point in each case. This
     * is made more complex by the fact that the "appropriate place" in some
     * cases is well inside the trace_event function, but that function is
     * invoked here and also invoked directly from interrupt code through
     * inside_trace_event.
     */
    switch (_TRACE_GET_FLAG(cmd[0])) {
    case _TRACE_GET_FLAG(RTMUX_TRACE_ADDALLCLASSES): /* Adding all classes */
    {
        unsigned i;

        int_enter_enable_mask = (0xffffffff);
        int_exit_enable_mask = (0xffffffff);
        trace_masks.control_mask = (0xffffffff);
        trace_masks.pr_mask = (0xffffffff);
        trace_masks.th_mask |= ~_TRACE_THREAD_ARG_WIDE;
        trace_masks.vth_mask = (0xffffffff);

        for (i = 0; i < _TRACE_MAX_USER_NUM; ++i) {
            trace_masks.user_mask[i] |= _TRACE_ENTER_USER;
        }

        for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
            trace_masks.system_mask[i] |= _TRACE_ENTER_SYSTEM;
        }

        for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
            trace_masks.comm_mask[i] |= _TRACE_ENTER_COMM;
        }

        for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
            trace_masks.int_masks[i] |= (_TRACE_ENTER_INT | _TRACE_EXIT_INT);
            trace_masks.int_handler_masks[i] |=
                (_TRACE_ENTER_INT | _TRACE_EXIT_INT);
        }

        for (i = 0; i < ksys_call_entry_num; ++i) {
            trace_masks.ksys_call_masks[i] |=
                (_TRACE_ENTER_CALL | _TRACE_EXIT_CALL);
        }

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_DELALLCLASSES): /* Deleting all classes */
    {
        unsigned i;

        int_enter_enable_mask = (0x00000000);
        int_exit_enable_mask = (0x00000000);
        trace_masks.control_mask = (0x00000000);
        trace_masks.pr_mask = (0x00000000);
        trace_masks.th_mask &= _TRACE_THREAD_ARG_WIDE;
        trace_masks.vth_mask = (0x00000000);

        for (i = 0; i < _TRACE_MAX_USER_NUM; ++i) {
            trace_masks.user_mask[i] &= ~(_TRACE_ENTER_USER);
        }

        for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
            trace_masks.system_mask[i] &= ~(_TRACE_ENTER_SYSTEM);
        }

        for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
            trace_masks.comm_mask[i] &= ~(_TRACE_ENTER_COMM);
        }

        for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
            trace_masks.int_masks[i] &= ~(_TRACE_ENTER_INT | _TRACE_EXIT_INT);
            trace_masks.int_handler_masks[i] &=
                ~(_TRACE_ENTER_INT | _TRACE_EXIT_INT);
        }

        for (i = 0; i < ksys_call_entry_num; ++i) {
            trace_masks.ksys_call_masks[i] &=
                ~(_TRACE_ENTER_CALL | _TRACE_EXIT_CALL);
        }

        return EOK;
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_SETCLASSPID): /* Selecting class process ID */
    {
        unsigned class = *(++c_p);
        unsigned pid = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL:
        case RTMUX_TRACE_KCALLENTER:
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_pid[i] = pid;
                trace_masks.ksys_call_tid[i] = 0;
            }

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_TH_STATE_NUM; ++i) {
                trace_masks.thread_pid[i] = pid;
                trace_masks.thread_tid[i] = 0;
            }

            return EOK;
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_TH_STATE_NUM; ++i) {
                trace_masks.vthread_pid[i] = pid;
                trace_masks.vthread_tid[i] = 0;
            }

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
                trace_masks.system_pid[i] = pid;
                trace_masks.system_tid[i] = 0;
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
                trace_masks.comm_pid[i] = pid;
                trace_masks.comm_tid[i] = 0;
            }

            return EOK;
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_SETCLASSTID): /* Selecting class thread ID */
    {
        unsigned class = *(++c_p);
        unsigned pid = *(++c_p);
        unsigned tid = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL:
        case RTMUX_TRACE_KCALLENTER:
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_pid[i] = pid;
                trace_masks.ksys_call_tid[i] = tid;
            }

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_TH_STATE_NUM; ++i) {
                trace_masks.thread_pid[i] = pid;
                trace_masks.thread_tid[i] = tid;
            }

            return EOK;
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_TH_STATE_NUM; ++i) {
                trace_masks.vthread_pid[i] = pid;
                trace_masks.vthread_tid[i] = tid;
            }

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
                trace_masks.system_pid[i] = pid;
                trace_masks.system_tid[i] = tid;
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
                trace_masks.comm_pid[i] = pid;
                trace_masks.comm_tid[i] = tid;
            }

            return EOK;
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_CLRCLASSPID): /* Clearing class process ID */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL:
        case RTMUX_TRACE_KCALLENTER:
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                if (trace_masks.ksys_call_tid[i] == 0) {
                    trace_masks.ksys_call_pid[i] = 0;
                }
            }

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_TH_STATE_NUM; ++i) {
                if (trace_masks.thread_tid[i] == 0) {
                    trace_masks.thread_pid[i] = 0;
                }
            }

            return EOK;
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_TH_STATE_NUM; ++i) {
                if (trace_masks.vthread_tid[i] == 0) {
                    trace_masks.vthread_pid[i] = 0;
                }
            }

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
                if (trace_masks.system_tid[i] == 0) {
                    trace_masks.system_pid[i] = 0;
                }
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
                if (trace_masks.comm_tid[i] == 0) {
                    trace_masks.comm_pid[i] = 0;
                }
            }

            return EOK;
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_CLRCLASSTID): /* Clearing class thread ID */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL:
        case RTMUX_TRACE_KCALLENTER:
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                if (trace_masks.ksys_call_tid[i]) {
                    trace_masks.ksys_call_pid[i] = 0;
                    trace_masks.ksys_call_tid[i] = 0;
                }
            }

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_TH_STATE_NUM; ++i) {
                if (trace_masks.thread_tid[i]) {
                    trace_masks.thread_pid[i] = 0;
                    trace_masks.thread_tid[i] = 0;
                }
            }

            return EOK;
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_TH_STATE_NUM; ++i) {
                if (trace_masks.vthread_tid[i]) {
                    trace_masks.vthread_pid[i] = 0;
                    trace_masks.vthread_tid[i] = 0;
                }
            }

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
                if (trace_masks.system_tid[i] == 0) {
                    trace_masks.system_pid[i] = 0;
                    trace_masks.system_tid[i] = 0;
                }
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
                if (trace_masks.comm_tid[i] == 0) {
                    trace_masks.comm_pid[i] = 0;
                    trace_masks.comm_tid[i] = 0;
                }
            }

            return EOK;
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_SETEVENTPID): /* Selecting one event process ID */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL:
        case RTMUX_TRACE_KCALLENTER:
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned e = *(++c_p);

            if (e < ksys_call_entry_num) {
                trace_masks.ksys_call_pid[e] = *(++c_p);
                trace_masks.ksys_call_tid[e] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                trace_masks.thread_pid[i_2] = *(++c_p);
                trace_masks.thread_tid[i_2] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                trace_masks.vthread_pid[i_2] = *(++c_p);
                trace_masks.vthread_tid[i_2] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned e = *(++c_p);

            if (e < _TRACE_MAX_SYSTEM_NUM) {
                trace_masks.system_pid[e] = *(++c_p);
                trace_masks.system_tid[e] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_COMM: {
            unsigned e = *(++c_p);

            if (e < _TRACE_MAX_COMM_NUM) {
                trace_masks.comm_pid[e] = *(++c_p);
                trace_masks.comm_tid[e] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_SETEVENTTID): /* Selecting one event thread ID */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL:
        case RTMUX_TRACE_KCALLENTER:
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned e = *(++c_p);

            if (e < ksys_call_entry_num) {
                trace_masks.ksys_call_pid[e] = *(++c_p);
                trace_masks.ksys_call_tid[e] = *(++c_p);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                trace_masks.thread_pid[i_2] = *(++c_p);
                trace_masks.thread_tid[i_2] = *(++c_p);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                trace_masks.vthread_pid[i_2] = *(++c_p);
                trace_masks.vthread_tid[i_2] = *(++c_p);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned e = *(++c_p);

            if (e < _TRACE_MAX_SYSTEM_NUM) {
                trace_masks.system_pid[e] = *(++c_p);
                trace_masks.system_tid[e] = *(++c_p);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_COMM: {
            unsigned e = *(++c_p);

            if (e < _TRACE_MAX_COMM_NUM) {
                trace_masks.comm_pid[e] = *(++c_p);
                trace_masks.comm_tid[e] = *(++c_p);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_CLREVENTPID): /* Clearing one event process ID */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL:
        case RTMUX_TRACE_KCALLENTER:
        case RTMUX_TRACE_KCALLEXIT: {
            if ((*(++c_p)) < ksys_call_entry_num &&
                trace_masks.ksys_call_tid[*c_p] == 0) {
                trace_masks.ksys_call_pid[*c_p] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM &&
                trace_masks.thread_tid[i_2] == 0) {
                trace_masks.thread_pid[i_2] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM &&
                trace_masks.vthread_tid[i_2] == 0) {
                trace_masks.vthread_pid[i_2] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_SYSTEM: {
            if ((*(++c_p)) < _TRACE_MAX_SYSTEM_NUM &&
                trace_masks.system_tid[*c_p] == 0) {
                trace_masks.system_pid[*c_p] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_COMM: {
            if ((*(++c_p)) < _TRACE_MAX_COMM_NUM &&
                trace_masks.comm_tid[*c_p] == 0) {
                trace_masks.comm_pid[*c_p] = 0;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_CLREVENTTID): /* Clearing one event thread ID */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL:
        case RTMUX_TRACE_KCALLENTER:
        case RTMUX_TRACE_KCALLEXIT: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                if (trace_masks.ksys_call_tid[*c_p]) {
                    trace_masks.ksys_call_pid[*c_p] = 0;
                    trace_masks.ksys_call_tid[*c_p] = 0;
                }

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                if (trace_masks.thread_tid[i_2]) {
                    trace_masks.thread_pid[i_2] = 0;
                    trace_masks.thread_tid[i_2] = 0;
                }

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                if (trace_masks.vthread_tid[i_2]) {
                    trace_masks.vthread_pid[i_2] = 0;
                    trace_masks.vthread_tid[i_2] = 0;
                }

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_SYSTEM: {
            if ((*(++c_p)) < _TRACE_MAX_SYSTEM_NUM) {
                if (trace_masks.system_tid[*c_p]) {
                    trace_masks.system_pid[*c_p] = 0;
                    trace_masks.system_tid[*c_p] = 0;
                }

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_COMM: {
            if ((*(++c_p)) < _TRACE_MAX_COMM_NUM) {
                if (trace_masks.comm_tid[*c_p]) {
                    trace_masks.comm_pid[*c_p] = 0;
                    trace_masks.comm_tid[*c_p] = 0;
                }

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_SETALLCLASSESFAST): /* Setting all classes fast */
    {
        unsigned i;

        for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
            trace_masks.system_mask[i] &= ~_TRACE_SYSTEM_ARG_WIDE;
        }

        for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
            trace_masks.comm_mask[i] &= ~_TRACE_COMM_ARG_WIDE;
        }

        for (i = 0; i < ksys_call_entry_num; ++i) {
            trace_masks.ksys_call_masks[i] &=
                ~(_TRACE_CALL_ARG_WIDE | _TRACE_CALL_RET_WIDE);
        }

        trace_masks.th_mask &= ~_TRACE_THREAD_ARG_WIDE;

        return EOK;
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_SETALLCLASSESWIDE): /* Setting all classes wide */
    {
        unsigned i;

        for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
            trace_masks.system_mask[i] |= _TRACE_SYSTEM_ARG_WIDE;
        }

        for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
            trace_masks.comm_mask[i] |= _TRACE_COMM_ARG_WIDE;
        }

        for (i = 0; i < ksys_call_entry_num; ++i) {
            trace_masks.ksys_call_masks[i] |=
                (_TRACE_CALL_ARG_WIDE | _TRACE_CALL_RET_WIDE);
        }

        trace_masks.th_mask |= _TRACE_THREAD_ARG_WIDE;

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_ADDCLASS): /* Adding one class */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_USER: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_USER_NUM; ++i) {
                trace_masks.user_mask[i] |= (_TRACE_ENTER_USER);
            }

            return EOK;
        }
        case RTMUX_TRACE_CONTROL: {
            trace_masks.control_mask = (0xffffffff);

            return EOK;
        }
        case RTMUX_TRACE_KCALL: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] |=
                    (_TRACE_ENTER_CALL | _TRACE_EXIT_CALL);
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLENTER: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] |= _TRACE_ENTER_CALL;
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] |= _TRACE_EXIT_CALL;
            }

            return EOK;
        }
        case RTMUX_TRACE_INT: {
            unsigned i;

            int_enter_enable_mask = (0xffffffff);
            int_exit_enable_mask = (0xffffffff);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_masks[i] |=
                    (_TRACE_ENTER_INT | _TRACE_EXIT_INT);
                trace_masks.int_handler_masks[i] |=
                    (_TRACE_ENTER_INT | _TRACE_EXIT_INT);
            }

            return EOK;
        }
        case RTMUX_TRACE_INTENTER: {
            unsigned i;

            int_enter_enable_mask = (0xffffffff);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_masks[i] |= _TRACE_ENTER_INT;
            }

            return EOK;
        }
        case RTMUX_TRACE_INTEXIT: {
            unsigned i;

            int_exit_enable_mask = (0xffffffff);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_masks[i] |= _TRACE_EXIT_INT;
            }

            return EOK;
        }
        case RTMUX_TRACE_INT_HANDLER_ENTER: {
            unsigned i;

            int_enter_enable_mask = (0xffffffff);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_handler_masks[i] |= _TRACE_ENTER_INT;
            }

            return EOK;
        }
        case RTMUX_TRACE_INT_HANDLER_EXIT: {
            unsigned i;

            int_exit_enable_mask = (0xffffffff);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_handler_masks[i] |= _TRACE_EXIT_INT;
            }

            return EOK;
        }
        case RTMUX_TRACE_PROCESS: {
            trace_masks.pr_mask = (0xffffffff);

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            trace_masks.th_mask = (0xffffffff);

            return EOK;
        }
        case RTMUX_TRACE_VTHREAD: {
            trace_masks.vth_mask = (0xffffffff);

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
                trace_masks.system_mask[i] |= _TRACE_ENTER_SYSTEM;
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
                trace_masks.comm_mask[i] |= _TRACE_ENTER_COMM;
            }

            return EOK;
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_DELCLASS): /* Deleting one class */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_USER: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_USER_NUM; ++i) {
                trace_masks.user_mask[i] &= ~(_TRACE_ENTER_USER);
            }

            return EOK;
        }
        case RTMUX_TRACE_CONTROL: {
            trace_masks.control_mask = (0x00000000);

            return EOK;
        }
        case RTMUX_TRACE_KCALL: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] &=
                    ~(_TRACE_ENTER_CALL | _TRACE_EXIT_CALL);
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLENTER: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] &= ~(_TRACE_ENTER_CALL);
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] &= ~(_TRACE_EXIT_CALL);
            }

            return EOK;
        }
        case RTMUX_TRACE_INT: {
            unsigned i;

            int_enter_enable_mask = (0x00000000);
            int_exit_enable_mask = (0x00000000);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_masks[i] &=
                    ~(_TRACE_ENTER_INT | _TRACE_EXIT_INT);
                trace_masks.int_handler_masks[i] &=
                    ~(_TRACE_ENTER_INT | _TRACE_EXIT_INT);
            }

            return EOK;
        }
        case RTMUX_TRACE_INTENTER: {
            unsigned i;

            int_enter_enable_mask = (0x00000000);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_masks[i] &= ~(_TRACE_ENTER_INT);
            }

            return EOK;
        }
        case RTMUX_TRACE_INTEXIT: {
            unsigned i;

            int_exit_enable_mask = (0x00000000);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_masks[i] &= ~(_TRACE_EXIT_INT);
            }

            return EOK;
        }
        case RTMUX_TRACE_INT_HANDLER_ENTER: {
            unsigned i;

            int_enter_enable_mask = (0x00000000);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_handler_masks[i] &= ~(_TRACE_ENTER_INT);
            }

            return EOK;
        }
        case RTMUX_TRACE_INT_HANDLER_EXIT: {
            unsigned i;

            int_exit_enable_mask = (0x00000000);
            for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                trace_masks.int_handler_masks[i] &= ~(_TRACE_EXIT_INT);
            }

            return EOK;
        }
        case RTMUX_TRACE_PROCESS: {
            trace_masks.pr_mask = (0x00000000);

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            trace_masks.th_mask = (0x00000000);

            return EOK;
        }
        case RTMUX_TRACE_VTHREAD: {
            trace_masks.vth_mask = (0x00000000);

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
                trace_masks.system_mask[i] &= ~(_TRACE_ENTER_SYSTEM);
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
                trace_masks.comm_mask[i] &= ~(_TRACE_ENTER_COMM);
            }

            return EOK;
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_SETCLASSFAST): /* Setting fast one class */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] &=
                    ~(_TRACE_CALL_ARG_WIDE | _TRACE_CALL_RET_WIDE);
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLENTER: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] &= ~(_TRACE_CALL_ARG_WIDE);
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] &= ~(_TRACE_CALL_RET_WIDE);
            }

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
                trace_masks.system_mask[i] &= ~_TRACE_SYSTEM_ARG_WIDE;
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
                trace_masks.comm_mask[i] &= ~_TRACE_COMM_ARG_WIDE;
            }

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            trace_masks.th_mask &= ~_TRACE_THREAD_ARG_WIDE;
            return EOK;
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_SETCLASSWIDE): /* Setting wide one class */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] |=
                    (_TRACE_CALL_ARG_WIDE | _TRACE_CALL_RET_WIDE);
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLENTER: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] |= _TRACE_CALL_ARG_WIDE;
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLEXIT: {
            unsigned i;

            for (i = 0; i < ksys_call_entry_num; ++i) {
                trace_masks.ksys_call_masks[i] |= _TRACE_CALL_RET_WIDE;
            }

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_SYSTEM_NUM; ++i) {
                trace_masks.system_mask[i] |= _TRACE_SYSTEM_ARG_WIDE;
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            unsigned i;

            for (i = 0; i < _TRACE_MAX_COMM_NUM; ++i) {
                trace_masks.comm_mask[i] |= _TRACE_COMM_ARG_WIDE;
            }

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            trace_masks.th_mask |= _TRACE_THREAD_ARG_WIDE;
            return EOK;
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_ADDEVENT): /* Add one event */
    {
        switch (*(++c_p)) {
        case RTMUX_TRACE_EMPTY: {
            return EOK;
        }
        case RTMUX_TRACE_USER: {
            uint32_t code = *(++c_p);
            if (code > RTMUX_TRACE_USERLAST) {
                return EINVAL;
            }
            trace_masks.user_mask[code] |= (_TRACE_ENTER_USER);

            return EOK;
        }
        case RTMUX_TRACE_CONTROL: {
            trace_masks.control_mask |= *(++c_p);

            return EOK;
        }
        case RTMUX_TRACE_KCALL: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] |=
                    (_TRACE_ENTER_CALL | _TRACE_EXIT_CALL);
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLENTER: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] |= _TRACE_ENTER_CALL;
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLEXIT: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] |= _TRACE_EXIT_CALL;
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        case RTMUX_TRACE_INT: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                int_enter_enable_mask = (0xffffffff);
                int_exit_enable_mask = (0xffffffff);
                trace_masks.int_masks[l] |=
                    (_TRACE_ENTER_INT | _TRACE_EXIT_INT);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INTENTER: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                int_enter_enable_mask = (0xffffffff);
                trace_masks.int_masks[l] |= _TRACE_ENTER_INT;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INTEXIT: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                int_exit_enable_mask = (0xffffffff);
                trace_masks.int_masks[l] |= _TRACE_EXIT_INT;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INT_HANDLER_ENTER: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                int_enter_enable_mask = (0xffffffff);
                trace_masks.int_handler_masks[l] |= _TRACE_ENTER_INT;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INT_HANDLER_EXIT: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                int_exit_enable_mask = (0xffffffff);
                trace_masks.int_handler_masks[l] |= _TRACE_EXIT_INT;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_PROCESS: {
            trace_masks.pr_mask |= *(++c_p);

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            trace_masks.th_mask |= *(++c_p);

            return EOK;
        }
        case RTMUX_TRACE_VTHREAD: {
            trace_masks.vth_mask |= *(++c_p);

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            if ((*(++c_p)) < _TRACE_MAX_SYSTEM_NUM) {
                trace_masks.system_mask[*c_p] |= _TRACE_ENTER_SYSTEM;
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            if ((*(++c_p)) < _TRACE_MAX_COMM_NUM) {
                trace_masks.comm_mask[*c_p] |= _TRACE_ENTER_COMM;
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_DELEVENT): /* Deleting one event */
    {
        switch (*(++c_p)) {
        case RTMUX_TRACE_EMPTY: {
            return EOK;
        }
        case RTMUX_TRACE_USER: {
            uint32_t code = *(++c_p);
            if (code > RTMUX_TRACE_USERLAST) {
                return EINVAL;
            }
            trace_masks.user_mask[code] &= ~(_TRACE_ENTER_USER);

            return EOK;
        }
        case RTMUX_TRACE_CONTROL: {
            trace_masks.control_mask &= ~(*(++c_p));

            return EOK;
        }
        case RTMUX_TRACE_KCALL: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] &=
                    ~(_TRACE_ENTER_CALL | _TRACE_EXIT_CALL);
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLENTER: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] &= ~(_TRACE_ENTER_CALL);
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        case RTMUX_TRACE_KCALLEXIT: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] &= ~(_TRACE_EXIT_CALL);
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        case RTMUX_TRACE_INT: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                unsigned i, j;

                trace_masks.int_masks[l] &=
                    ~(_TRACE_ENTER_INT | _TRACE_EXIT_INT);
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_ENTER_INT & trace_masks.int_masks[i]) {
                        break;
                    }
                }
                for (j = 0; j < _TRACE_MAX_INT_NUM; ++j) {
                    if (_TRACE_ENTER_INT & trace_masks.int_handler_masks[j]) {
                        break;
                    }
                }
                if (i == _TRACE_MAX_INT_NUM && j == _TRACE_MAX_INT_NUM) {
                    int_enter_enable_mask = (0x00000000);
                }
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_EXIT_INT & trace_masks.int_masks[i]) {
                        break;
                    }
                }
                for (j = 0; j < _TRACE_MAX_INT_NUM; ++j) {
                    if (_TRACE_EXIT_INT & trace_masks.int_handler_masks[j]) {
                        break;
                    }
                }
                if (i == _TRACE_MAX_INT_NUM && j == _TRACE_MAX_INT_NUM) {
                    int_exit_enable_mask = (0x00000000);
                }

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INTENTER: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                unsigned i;

                trace_masks.int_masks[l] &= ~(_TRACE_ENTER_INT);
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_ENTER_INT & trace_masks.int_masks[i]) {
                        return EOK;
                    }
                }
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_ENTER_INT & trace_masks.int_handler_masks[i]) {
                        return EOK;
                    }
                }
                int_enter_enable_mask = (0x00000000);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INTEXIT: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                unsigned i;

                trace_masks.int_masks[l] &= ~(_TRACE_EXIT_INT);
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_EXIT_INT & trace_masks.int_masks[i]) {
                        return EOK;
                    }
                }
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_EXIT_INT & trace_masks.int_handler_masks[i]) {
                        return EOK;
                    }
                }
                int_exit_enable_mask = (0x00000000);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INT_HANDLER_ENTER: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                unsigned i;

                trace_masks.int_handler_masks[l] &= ~(_TRACE_ENTER_INT);
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_ENTER_INT & trace_masks.int_handler_masks[i]) {
                        return EOK;
                    }
                }
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_ENTER_INT & trace_masks.int_masks[i]) {
                        return EOK;
                    }
                }
                int_enter_enable_mask = (0x00000000);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INT_HANDLER_EXIT: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                unsigned i;

                trace_masks.int_handler_masks[l] &= ~(_TRACE_EXIT_INT);
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_EXIT_INT & trace_masks.int_handler_masks[i]) {
                        return EOK;
                    }
                }
                for (i = 0; i < _TRACE_MAX_INT_NUM; ++i) {
                    if (_TRACE_EXIT_INT & trace_masks.int_masks[i]) {
                        return EOK;
                    }
                }
                int_exit_enable_mask = (0x00000000);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_PROCESS: {
            trace_masks.pr_mask &= ~(*(++c_p));

            return EOK;
        }
        case RTMUX_TRACE_THREAD: {
            trace_masks.th_mask &= ~(*(++c_p));

            return EOK;
        }
        case RTMUX_TRACE_VTHREAD: {
            trace_masks.vth_mask &= ~(*(++c_p));

            return EOK;
        }
        case RTMUX_TRACE_SYSTEM: {
            if ((*(++c_p)) < _TRACE_MAX_SYSTEM_NUM) {
                trace_masks.system_mask[*c_p] &= ~(_TRACE_ENTER_SYSTEM);
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        case RTMUX_TRACE_COMM: {
            if ((*(++c_p)) < _TRACE_MAX_COMM_NUM) {
                trace_masks.comm_mask[*c_p] &= ~(_TRACE_ENTER_COMM);
            } else {
                return ENOTSUP;
            }

            return EOK;
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_SETEVENTFAST): /* Setting one event fast */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] &=
                    ~(_TRACE_CALL_ARG_WIDE | _TRACE_CALL_RET_WIDE);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_KCALLENTER: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] &= ~(_TRACE_CALL_ARG_WIDE);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_KCALLEXIT: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] &= ~(_TRACE_CALL_RET_WIDE);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_COMM: {
            if ((*(++c_p)) < _TRACE_MAX_COMM_NUM) {
                trace_masks.comm_mask[*c_p] &= ~_TRACE_COMM_ARG_WIDE;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_SYSTEM: {
            if ((*(++c_p)) < _TRACE_MAX_SYSTEM_NUM) {
                trace_masks.system_mask[*c_p] &= ~_TRACE_SYSTEM_ARG_WIDE;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_SETEVENTWIDE): /* Setting one event wide */
    {
        unsigned class = *(++c_p);

        switch (class) {
        case RTMUX_TRACE_KCALL: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] |=
                    (_TRACE_CALL_ARG_WIDE | _TRACE_CALL_RET_WIDE);

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_KCALLENTER: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] |= _TRACE_CALL_ARG_WIDE;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_KCALLEXIT: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                trace_masks.ksys_call_masks[*c_p] |= _TRACE_CALL_RET_WIDE;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_COMM: {
            if ((*(++c_p)) < _TRACE_MAX_COMM_NUM) {
                trace_masks.comm_mask[*c_p] |= _TRACE_COMM_ARG_WIDE;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_SYSTEM: {
            if ((*(++c_p)) < _TRACE_MAX_SYSTEM_NUM) {
                trace_masks.system_mask[*c_p] |= _TRACE_SYSTEM_ARG_WIDE;

                return EOK;
            } else {
                return ENOTSUP;
            }
        }
        default: {
            return check_class(class);
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_ADDEVENTHANDLER): /* Adding one event handler */
    {
        if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
            !(act->process->priv_caps & RTMUX_PRIV_CAP_TRACE) ||
            !(act->flags & RTMUX_FLG_THR_IOPRIV))
            return EPERM;
        if (trace_masks.eh_num >= _TRACE_MAX_EV_HANDLER_NUM) {
            return ENOTSUP;
        }
        switch (*(++c_p)) {
        case RTMUX_TRACE_EMPTY: {
            return EOK;
        }
        case RTMUX_TRACE_CONTROL: {
            switch (*(++c_p)) {
            case RTMUX_TRACE_CONTROLTIME: {
                return _TRACE_ADD_EH(&trace_masks.control_time_ehd_p);
            }
            default: {
                return ENOTSUP;
            }
            }
        }
        case RTMUX_TRACE_KCALLENTER: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                return _TRACE_ADD_EH(trace_masks.ksys_call_enter_ehd_p + *c_p);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_KCALLEXIT: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                return _TRACE_ADD_EH(trace_masks.ksys_call_exit_ehd_p + *c_p);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INTENTER: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                return _TRACE_ADD_EH(trace_masks.int_enter_ehd_p + l);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INTEXIT: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                return _TRACE_ADD_EH(trace_masks.int_exit_ehd_p + l);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_PROCESS: {
            switch (*(++c_p)) {
            case RTMUX_TRACE_PROCCREATE: {
                return _TRACE_ADD_EH(&trace_masks.pr_create_ehd_p);
            }
            case RTMUX_TRACE_PROCDESTROY: {
                return _TRACE_ADD_EH(&trace_masks.pr_destroy_ehd_p);
            }
            case RTMUX_TRACE_PROCCREATE_NAME: {
                return _TRACE_ADD_EH(&trace_masks.pr_create_name_ehd_p);
            }
            case RTMUX_TRACE_PROCDESTROY_NAME: {
                return _TRACE_ADD_EH(&trace_masks.pr_destroy_name_ehd_p);
            }
            case RTMUX_TRACE_PROCTHREAD_NAME: {
                return _TRACE_ADD_EH(&trace_masks.th_name_ehd_p);
            }
            default: {
                return ENOTSUP;
            }
            }
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                return _TRACE_ADD_EH(trace_masks.thread_ehd_p + i_2);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                return _TRACE_ADD_EH(trace_masks.vthread_ehd_p + i_2);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_SYSTEM: {
            if ((*(++c_p)) < _TRACE_MAX_SYSTEM_NUM) {
                return _TRACE_ADD_EH(trace_masks.system_ehd_p + *c_p);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_COMM: {
            if ((*(++c_p)) < _TRACE_MAX_COMM_NUM) {
                return _TRACE_ADD_EH(trace_masks.comm_ehd_p + *c_p);
            } else {
                return ENOTSUP;
            }
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_DELEVENTHANDLER): /* Deleting one event handler */
    {
        switch (*(++c_p)) {
        case RTMUX_TRACE_EMPTY: {
            return EOK;
        }
        case RTMUX_TRACE_CONTROL: {
            switch (*(++c_p)) {
            case RTMUX_TRACE_CONTROLTIME: {
                return delete_eh(&trace_masks.control_time_ehd_p);
            }
            default: {
                return ENOTSUP;
            }
            }
        }
        case RTMUX_TRACE_KCALLENTER: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                return delete_eh(trace_masks.ksys_call_enter_ehd_p + *c_p);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_KCALLEXIT: {
            if ((*(++c_p)) < ksys_call_entry_num) {
                return delete_eh(trace_masks.ksys_call_exit_ehd_p + *c_p);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INTENTER: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                return delete_eh(trace_masks.int_enter_ehd_p + l);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_INTEXIT: {
            int l = get_int_level(*(++c_p));

            if (l > (-1) && l < _TRACE_MAX_INT_NUM) {
                return delete_eh(trace_masks.int_exit_ehd_p + l);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_PROCESS: {
            switch (*(++c_p)) {
            case RTMUX_TRACE_PROCCREATE: {
                return delete_eh(&trace_masks.pr_create_ehd_p);
            }
            case RTMUX_TRACE_PROCDESTROY: {
                return delete_eh(&trace_masks.pr_destroy_ehd_p);
            }
            case RTMUX_TRACE_PROCCREATE_NAME: {
                return delete_eh(&trace_masks.pr_create_name_ehd_p);
            }
            case RTMUX_TRACE_PROCDESTROY_NAME: {
                return delete_eh(&trace_masks.pr_destroy_name_ehd_p);
            }
            case RTMUX_TRACE_PROCTHREAD_NAME: {
                return delete_eh(&trace_masks.th_name_ehd_p);
            }
            default: {
                return ENOTSUP;
            }
            }
        }
        case RTMUX_TRACE_THREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                return delete_eh(trace_masks.thread_ehd_p + i_2);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_VTHREAD: {
            unsigned i_1 = *(++c_p);
            unsigned i_2 = 0U;

            for (i_1 = i_1 >> 1; i_1; i_1 = i_1 >> 1, ++i_2) {
                /* nothing to do */
            }
            if (i_2 < _TRACE_MAX_TH_STATE_NUM) {
                return delete_eh(trace_masks.vthread_ehd_p + i_2);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_SYSTEM: {
            if ((*(++c_p)) < _TRACE_MAX_SYSTEM_NUM) {
                return delete_eh(trace_masks.system_ehd_p + *c_p);
            } else {
                return ENOTSUP;
            }
        }
        case RTMUX_TRACE_COMM: {
            if ((*(++c_p)) < _TRACE_MAX_COMM_NUM) {
                return delete_eh(trace_masks.comm_ehd_p + *c_p);
            } else {
                return ENOTSUP;
            }
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_ADDCLASSEVHANDLER): /* Adding one event handler */
    {
        if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
            !(act->process->priv_caps & RTMUX_PRIV_CAP_TRACE) ||
            !(act->flags & RTMUX_FLG_THR_IOPRIV))
            return EPERM;
        if (trace_masks.eh_num >= _TRACE_MAX_EV_HANDLER_NUM) {
            return ENOTSUP;
        }
        switch (*(++c_p)) {
        case RTMUX_TRACE_EMPTY: {
            return EOK;
        }
        case RTMUX_TRACE_CONTROL: {
            return _TRACE_ADD_EH(&trace_masks.class_control_ehd_p);
        }
        case RTMUX_TRACE_KCALL: {
            return (
                _TRACE_DOUBLE_ADD_EH(&trace_masks.class_ker_call_enter_ehd_p,
                                     &trace_masks.class_ker_call_exit_ehd_p));
        }
        case RTMUX_TRACE_KCALLENTER: {
            return _TRACE_ADD_EH(&trace_masks.class_ker_call_enter_ehd_p);
        }
        case RTMUX_TRACE_KCALLEXIT: {
            return _TRACE_ADD_EH(&trace_masks.class_ker_call_exit_ehd_p);
        }
        case RTMUX_TRACE_INT: {
            return (_TRACE_DOUBLE_ADD_EH(&trace_masks.class_int_enter_ehd_p,
                                         &trace_masks.class_int_exit_ehd_p));
        }
        case RTMUX_TRACE_INTENTER: {
            return _TRACE_ADD_EH(&trace_masks.class_int_enter_ehd_p);
        }
        case RTMUX_TRACE_INTEXIT: {
            return _TRACE_ADD_EH(&trace_masks.class_int_exit_ehd_p);
        }
        case RTMUX_TRACE_PROCESS: {
            return _TRACE_ADD_EH(&trace_masks.class_process_ehd_p);
        }
        case RTMUX_TRACE_THREAD: {
            return _TRACE_ADD_EH(&trace_masks.class_thread_ehd_p);
        }
        case RTMUX_TRACE_VTHREAD: {
            return _TRACE_ADD_EH(&trace_masks.class_vthread_ehd_p);
        }
        case RTMUX_TRACE_SYSTEM: {
            return _TRACE_ADD_EH(&trace_masks.class_system_ehd_p);
        }
        case RTMUX_TRACE_COMM: {
            return _TRACE_ADD_EH(&trace_masks.class_comm_ehd_p);
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_DELCLASSEVHANDLER): /* Deleting one event handler */
    {
        switch (*(++c_p)) {
        case RTMUX_TRACE_EMPTY: {
            return EOK;
        }
        case RTMUX_TRACE_CONTROL: {
            return delete_eh(&trace_masks.class_control_ehd_p);
        }
        case RTMUX_TRACE_KCALL: {
            (void) delete_eh(&trace_masks.class_ker_call_enter_ehd_p);
            (void) delete_eh(&trace_masks.class_ker_call_exit_ehd_p);

            return EOK;
        }
        case RTMUX_TRACE_KCALLENTER: {
            return delete_eh(&trace_masks.class_ker_call_enter_ehd_p);
        }
        case RTMUX_TRACE_KCALLEXIT: {
            return delete_eh(&trace_masks.class_ker_call_exit_ehd_p);
        }
        case RTMUX_TRACE_INT: {
            (void) delete_eh(&trace_masks.class_int_enter_ehd_p);
            (void) delete_eh(&trace_masks.class_int_exit_ehd_p);

            return EOK;
        }
        case RTMUX_TRACE_INTENTER: {
            return delete_eh(&trace_masks.class_int_enter_ehd_p);
        }
        case RTMUX_TRACE_INTEXIT: {
            return delete_eh(&trace_masks.class_int_exit_ehd_p);
        }
        case RTMUX_TRACE_PROCESS: {
            return delete_eh(&trace_masks.class_process_ehd_p);
        }
        case RTMUX_TRACE_THREAD: {
            return delete_eh(&trace_masks.class_thread_ehd_p);
        }
        case RTMUX_TRACE_VTHREAD: {
            return delete_eh(&trace_masks.class_vthread_ehd_p);
        }
        case RTMUX_TRACE_SYSTEM: {
            return delete_eh(&trace_masks.class_system_ehd_p);
        }
        case RTMUX_TRACE_COMM: {
            return delete_eh(&trace_masks.class_comm_ehd_p);
        }
        default: {
            return ENOTSUP;
        }
        }
    }
    /* One arm: the two modes differed only in the flag word they stamp into
     * each buffer header, and duplicating the walk duplicated the locking too.
     */
    case _TRACE_GET_FLAG(RTMUX_TRACE_SETRINGMODE):   /* Set ring mode */
    case _TRACE_GET_FLAG(RTMUX_TRACE_SETLINEARMODE): /* Set linear mode */
    {
        const uint32_t ring = (_TRACE_GET_FLAG(cmd[0]) ==
                               _TRACE_GET_FLAG(RTMUX_TRACE_SETRINGMODE))
                                  ? 1u
                                  : 0u;
        tracebuf_t *l_b_p;
        tracebuf_t *b_p;

        /* Same authority as ALLOCBUFFER and DEALLOCBUFFER. These two arms
         * reset every buffer in the ring and flip global trace state, which is
         * the tracer's authority, not any caller's; the asymmetry where the
         * freer needed RTMUX_PRIV_CAP_TRACE and these did not was itself the
         * bug. It also bounds who can hold the interrupts-off window below: the
         * walk is O(buff_num), so an ungated caller could keep interrupts off
         * on its hart for a full 1024-buffer ring on every call and blow the
         * interrupt-latency figures in docs/rt-budgets.md. No in-tree caller
         * issues these opcodes without the cap; tracelogger already holds it.
         */
        if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
            !(act->process->priv_caps & RTMUX_PRIV_CAP_TRACE))
            return EPERM;

        /* state_spin held across the whole walk. privateptr->tracebuf used to
         * be read and dereferenced with no lock at all, racing DEALLOCBUFFER's
         * store of NULL. interrupt_disable + SPINLOCK rather than
         * interrupt_lock, matching add_trace_event: the per-buffer lock nests
         * inside this one, and interrupt_unlock would re-enable interrupts
         * while state_spin is still held. ALLOCBUFFER now publishes the ring
         * under this same lock only after linking it, so the walk cannot meet a
         * NULL h.next. DEALLOCBUFFER's pa_free still lands outside this lock,
         * which is the buffers-in-use FIX_ME in its own arm, not this one.
         */
        interrupt_disable();
        SPINLOCK(&trace_masks.state_spin);
        l_b_p = privateptr->tracebuf;
        if (trace_masks.main_flags || !l_b_p) {
            SPINUNLOCK(&trace_masks.state_spin);
            interrupt_enable();
            return ECANCELED;
        }
        trace_masks.ring_mode = ring;
        b_p = l_b_p;
        do {
            SPINLOCK(&b_p->h.spin);
            b_p->h.tail_ptr = b_p->h.begin_ptr;
            b_p->h.num_events = 0;
            b_p->h.flags = ring ? _TRACE_FLAGS_RING : 0u;
            SPINUNLOCK(&b_p->h.spin);
            b_p = b_p->h.next;
        } while (l_b_p != b_p);
        SPINUNLOCK(&trace_masks.state_spin);
        interrupt_enable();

        return EOK;
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_INSERTSCLASSEVENT): /* Insert user simple class event */
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_INSERTCCLASSEVENT): /* Insert user combine class event */
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_INSERTSUSEREVENT): /* Insert user simple class event */
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_INSERTCUSEREVENT): /* Insert user combine class event */
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_INSERTUSRSTREVENT): /* Insert user string class event */
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_INSERTEVENT): /* Insert "row" user trace event */
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_STARTNOSTATE): /* Begin kernel tracing, no initial state */
    case _TRACE_GET_FLAG(RTMUX_TRACE_START): /* Begin kernel tracing */
    case _TRACE_GET_FLAG(RTMUX_TRACE_STOP):  /* Stop kernel tracing */
    {
        return trace_event(c_p);
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_ALLOCBUFFER): /* Allocating kernel trace buffer */
    {
        unsigned b_n = *++c_p;
        size_t buflen;
        size_t i;
        paddr_t paddr = 0;
        struct pa_quantum *pq;
        tracebuf_t *ptbuf;
        tracebuf_t *base;
        void *paddr_out;
        void *nullp = NULL;
        struct user_slice paddr_slice;

        if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
            !(act->process->priv_caps & RTMUX_PRIV_CAP_TRACE))
            return EPERM;

        /* b_n was taken from the caller unchecked and multiplied in 32-bit
         * arithmetic, so b_n near 0x40001 wrapped to one buffer's worth of
         * allocation while the header-init loop below walked ~16 GiB past it,
         * and b_n == 0 allocated nothing and then let `(--ptbuf)->h.next` store
         * below the allocation. Holding RTMUX_PRIV_CAP_TRACE buys a trace
         * buffer, not an arbitrary kernel write.
         */
        if (b_n == 0 || b_n > _TRACE_MAX_BUFFERS)
            return EINVAL;
        buflen = (size_t) b_n * _TRACEBUFSIZE;

        /* words 2 and 3 hold the caller's paddr-out pointer; the kernel writes
         * through it, so bounds-check the target before dereferencing. Holding
         * RTMUX_PRIV_CAP_TRACE buys the operation, not the right to name a
         * kernel address as the destination.
         */
        paddr_out = *(void **) ++c_p;
        if (user_slice_init(act, paddr_out, sizeof paddr, sizeof paddr,
                            USER_SLICE_WR, &paddr_slice) ||
            user_slice_copyout(&paddr_slice, &nullp))
            return EFAULT;
        interrupt_lock(&trace_masks.state_spin);
        if (trace_masks.buff_0_ptr) {
            interrupt_unlock(&trace_masks.state_spin);
            return EEXIST; /* buffer exist */
        }
        interrupt_unlock(&trace_masks.state_spin);

        /* Allocate user-mappable physical RAM, not kernel heap: tracelogger
         * maps this paddr from user space after ALLOCBUFFER returns.
         */
        pq = pa_alloc((paddr_t) buflen, 0, PAQ_COLOUR_NONE, PAA_FLAG_CONTIG,
                      NULL, restrict_user, 0);
        if (!pq) {
            ptbuf = NULL;
        } else {
            paddr = pa_quantum_to_paddr(pq);
            ptbuf = (tracebuf_t *) CPU_P2V(paddr);
            memset(ptbuf, 0, buflen);
        }
        interrupt_lock(&trace_masks.state_spin);
        if (trace_masks.buff_0_ptr) {
            interrupt_unlock(&trace_masks.state_spin);
            if (pq)
                pa_free(pq, LEN_TO_NQUANTUM(buflen), 0);
            return EEXIST; /* buffer exist */
        }
        if (!ptbuf) {
            interrupt_unlock(&trace_masks.state_spin);

            return ENOMEM;
        }
        trace_masks.buff_0_ptr = (void *) ptbuf;
        trace_masks.buff_num = b_n;
        trace_masks.max_events = (_TRACELEMENTS - _TRACEBUFEMPTY);
        tracebuf_pq = pq;
        interrupt_unlock(&trace_masks.state_spin);

        /* Initialize the ring before publishing it, not after. The store to
         * privateptr->tracebuf used to happen here, ahead of this loop, so a
         * reader that took state_spin in the window saw a non-NULL ring whose
         * headers were still the zeroed allocation: h.next was NULL, and the
         * mode setters walk exactly that link. Taking state_spin in the reader
         * serializes against DEALLOCBUFFER's store of NULL, not against a
         * publisher that publishes early. base is the ring head the headers
         * point at; nothing outside this loop can reach it until the store
         * below.
         */
        base = ptbuf;
        for (i = 0; i < b_n; ++i, ++ptbuf) { /* setup header information */
            ptbuf->h.flags = (uint32_t) 0;
            ptbuf->h.num_events = (uint32_t) 0;
            ptbuf->h.ls13_rb_num = (uint32_t) (i << 13);
            ptbuf->h.begin_ptr = (traceevent_t *) &ptbuf->data[0];
            ptbuf->h.tail_ptr = ptbuf->h.begin_ptr;
            ptbuf->h.baseaddr = base;
            ptbuf->h.next = ptbuf + 1;
            memset(&ptbuf->h.spin, 0, sizeof(ptbuf->h.spin));
        }
        (--ptbuf)->h.next = base;

        /* Publish under the same lock the readers hold, so a reader either
         * sees no ring or sees a fully linked one.
         */
        interrupt_lock(&trace_masks.state_spin);
        privateptr->tracebuf = base;
        interrupt_unlock(&trace_masks.state_spin);

        /* Fault-safe, for the same reason the pre-clear above is: the caller
         * can unmap this page during the pa_alloc and multi-megabyte memset
         * between the two writes, and a raw store would take an unrecoverable
         * S-mode fault with no handler armed. Byte-wise, so it also does not
         * depend on the caller's pointer being 8-byte aligned, which the trap
         * handler would not recover either.
         */
        if (user_slice_copyout(&paddr_slice, &paddr) != 0)
            return EFAULT;

        return EOK;
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_DEALLOCBUFFER): /* Deallocating trace buffer */
    {
        if (privateptr->tracebuf) {
            void *b_0_p;
            uint32_t b_n;

            if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
                !(act->process->priv_caps & RTMUX_PRIV_CAP_TRACE))
                return EPERM;
            trace_masks.main_flags = (uint32_t) 0;
            interrupt_lock(&trace_masks.state_spin);
            if (!(b_0_p = trace_masks.buff_0_ptr)) {
                trace_masks.buff_num = (uint32_t) 0;
                interrupt_unlock(&trace_masks.state_spin);
                return EOK;
            }
            b_n = trace_masks.buff_num;
            trace_masks.buff_0_ptr = NULL;
            privateptr->tracebuf = NULL;
            trace_masks.buff_num = (uint32_t) 0;
            interrupt_unlock(&trace_masks.state_spin);

            /* ///////////////////////////////////////////////////
             * //////////////////  FIX_ME!!  /////////////////////
             * There is a danger that deallocated buffers might be in use.
             * ///////////////////////////////////////////////////
             */

            (void) b_0_p;
            if (tracebuf_pq) {
                pa_free(tracebuf_pq, LEN_TO_NQUANTUM(b_n * _TRACEBUFSIZE), 0);
                tracebuf_pq = NULL;
            }
        }

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_FLUSHBUFFER): /* Flushing the buffer */
    {
        return trace_flushbuffer();
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_QUERYEVENTS): /* Getting event number */
    {
        tracebuf_t *tracebuf;
        uint32_t num_events;

        /* Gated like the mode setters: this reads the tracer's ring state. */
        if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
            !(act->process->priv_caps & RTMUX_PRIV_CAP_TRACE))
            return EPERM;

        /* Same race as the mode setters above: the read of
         * privateptr->tracebuf and the dereference that follows it must be one
         * critical section against DEALLOCBUFFER's store of NULL.
         */
        interrupt_lock(&trace_masks.state_spin);
        tracebuf = privateptr->tracebuf;
        num_events = tracebuf ? tracebuf->h.num_events : 0;
        interrupt_unlock(&trace_masks.state_spin);

        if (!tracebuf)
            return EFAULT;
        SETKSTATUS(act, num_events);

        return ENOERROR;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_SKIPBUFFER): /* Skip first buffers when */
    {                                             /* ring overflow */
        trace_masks.skip_buff = 1U;

        return EOK;
    }
    case _TRACE_GET_FLAG(
        RTMUX_TRACE_OVERWRITEBUFFER): /* Overwrite first buffer */
    {                                 /* when ring overflows */
        trace_masks.skip_buff = 0U;

        return EOK;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_QUERYVERSION): /* Query version */
    {
        SETKSTATUS(act, RTMUX_VERSION);

        return ENOERROR;
    }
    case _TRACE_GET_FLAG(RTMUX_TRACE_QUERYSUPPORT): /* Query support */
    {
        SETKSTATUS(act, RTMUX_TRACE_SUPPINSTR);

        return ENOERROR;
    }
    default: {
        return ENOTSUP;
    }
    }

    /* NOTREACHED */
    return ENOTSUP;
}

#else

/* Kernel handler for __KSYS_TRACE_EVENT when instrumentation is compiled out.
 * Only the RTMUX_TRACE_QUERY* flags are honoured; everything else is forwarded
 * to trace_event(). Parameters:
 *   act : calling thread
 *   kap : kernel args (control flag and operands at kap->data)
 * Return value: ENOERROR on a query, else trace_event()'s result
 */
int kdecl ksys_trace_event(thread_t *act, struct kcall_args_trace_event *kap)
{
    uint32_t cmd0;

    /* Trace is fully disabled here, so only the RTMUX_TRACE_QUERY* opcodes do
     * anything; everything else goes to the trace hook, which owns whatever it
     * makes of the caller's array.
     *
     * The opcode itself is read with the same fault-safe helper as the
     * instrumented variant: this stub is what a production kernel ships, and
     * trace_event is reachable by any process, so a bare probe of the caller's
     * pointer here was an unprivileged kernel crash.
     */
    if (trace_read_cmd(act, kap->data, &cmd0, 1) != EOK)
        return EFAULT;

    switch (_TRACE_GET_FLAG(cmd0)) {
    case _TRACE_GET_FLAG(RTMUX_TRACE_QUERYVERSION):
        SETKSTATUS(act, RTMUX_VERSION);
        return ENOERROR;
    case _TRACE_GET_FLAG(RTMUX_TRACE_QUERYSUPPORT):
        SETKSTATUS(act, RTMUX_TRACE_NOINSTRSUPP);
        return ENOERROR;
    default:
        return trace_event(kap->data);
    }
}

#endif
