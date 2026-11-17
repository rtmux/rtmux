/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <setjmp.h>
#include <core/globals.h>

#define XFER_IOV_NUM 32 /* Number of iov's to copy at a time */
#define XFER_PREEMPT(act)    \
    if (NEED_PREEMPT(act)) { \
        xfer_preempt(act);   \
    }

/* The xfer longjmp target is per-hart, reached via SET_XFER_ENV/GET_XFER_ENV
 * (macros.h). A shared global would let a fault on one hart longjmp into
 * another hart's stale xfer frame.
 */
jmp_buf *xfer_env[CONFIG_PROCESSORS_MAX];

#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)

/* Restore message-pass state after an xfer preemption/fault.
 *
 * Invoked from xfer-fault recovery (longjmp from xfer_*_fault). Releases the
 * MSG_DELIVERY claim on the active thread and, if a peer was mid delivery,
 * releases its claim too and force-readies it if it was waiting.
 *
 * "act" is this CPU's active thread, so its internal_flags / blocked_on writes
 * are single-writer-by-CPU; the cross-thread effect on the peer goes through
 * force_ready_msgxfer's IPI mechanism. Flag updates use atomic_clr for
 * cross-CPU safety. Parameters:
 *   act : the active thread whose xfer was interrupted
 */
static void xfer_restorestate(thread_t *act)
{
    thread_t *thp;

    if (act->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
        atomic_clr((volatile unsigned *) &act->internal_flags,
                   RTMUX_ITF_MSG_DELIVERY);

        if ((thp = act->blocked_on)) {
            act->blocked_on = 0;
            IPC_BLK(act);
            if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
                atomic_clr((volatile unsigned *) &thp->internal_flags,
                           RTMUX_ITF_MSG_DELIVERY);
                if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
                    force_ready(thp, (unsigned) KSTATUS(thp));
                    atomic_clr((volatile unsigned *) &thp->internal_flags,
                               RTMUX_ITF_MSG_FORCE_RDY);
                }
            }
        }
    }
}
#endif

/* Preempt a thread mid-xfer (the XFER_PREEMPT macro tail).
 *
 * Rewinds the thread's IP so the kernel call restarts (or unwinds the specret),
 * restores any interrupted message-pass state, and tail-calls kernel_exit
 * (which does not return). Parameters:
 *   thp : thread being preempted
 */
static void xfer_preempt(thread_t *thp)
{
    /* The specret-active flag is per-CPU (pc_in_specret). */
    if (pcpu_array[get_cpunum()].pc_in_specret) {
        unspecret_kernel();
    } else {
        /* KCALL_RESTART, not a bare rewind of the IP: a0 holds the first
         * argument only until something stores a status in it, and the call
         * must run again with the arguments it was entered with.
         */
        KCALL_RESTART(thp);
    }
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    xfer_restorestate(thp);
#endif
    SET_XFER_HANDLER(NULL);

    /* kernel_exit does not return to the trap handler that closes the rcvid
     * lookup window, so close it here: a call that restarts many times would
     * otherwise open more than the nesting counter can hold.
     */
    lookup_rcvid_release_all();
    kernel_exit();
}

/* This routine is called when the kernel wants to preempt a thread in the
 * middle of a message pass. The current position should be saved so when we
 * restart, it the message pass can continue where it left off. The restart must
 * verify that it is the origional threads to allow the restart to occur. On
 * entry:
 *   thp
 *      thread to preempt (May not be active)
 *
 *   thp->restart
 *      thread that was the source of the message pass
 *
 *   thp->restart->args.ms.msglen
 *      Number of bytes that have been sucessfully transfered
 *
 * On Exit:
 *   thp->restart->args.ms.msglen
 *      Updated using regs to more accurate number (if possible)
 */
void xfer_restart(thread_t *thp, CPU_REGISTERS *regs)
{
    if ((thp = thp->restart)) {
        thp->args.ms.msglen += xferiov_pos(regs);
    }
    kernel_exit();
}

/* Restart point for a preempted async message transfer.
 *
 * Advances the recorded transfer position by the bytes moved so far, then exits
 * the kernel to resume. Parameters:
 *   thp  : thread whose async transfer was preempted
 *   regs : register context (used to recover the transfer position)
 */
void xfer_async_restart(thread_t *thp, CPU_REGISTERS *regs)
{
    thp->args.ms.msglen += xferiov_pos(regs);
    kernel_exit();
}

/* This is the main message passing function.
 *
 * This function will map other processes address spaces as necessary to allow
 * the lower level xferiov() to transfer the data. It also converts negative
 * parts into a single iov, so the xferiov() doesn't have to worry about this.
 *
 * vm_ops->as() may be called to change the current address space for more
 * efficent transfers. The caller must be aware that when this function returns,
 * the address space may have been changed.
 *
 * vm_ops->map_xfer() will be called to map addresses. The first time it is
 * called, lo and hi will be zero. Each time it is called after that, it must be
 * called with the same prp and as_prp, and the previous lo and hi mappings to
 * allow VM to perform cleanup if necessary before mapping the new translation.
 * Before this function exits and if a non-zero translation from an
 * vm_ops->map_xfer() was returned, a zero sized call vm_ops->map_xfer() will be
 * called to let VM clean up.
 *
 * On entry:
 *   dthp
 *      Destination thread
 *
 *   dthp->args.ms.sparts
 *      If negative, if msg is a pointer to a buffer, else pointer to iov_t.
 *
 *   dthp->args.ms.smsg
 *      iov_t if sparts is positive, buffer if sparts is negative
 *
 *   doff
 *      Number of bytes to skip in destination before starting xfer
 *
 *   sthp
 *      Source thread
 *
 *   sthp->args.ms.sparts
 *      If negative, if msg is a pointer to a buffer, else pointer to iov_t.
 *
 *   sthp->args.ms.smsg
 *      iov_t if sparts is positive, buffer if sparts is negative
 *
 *   soff
 *      Number of bytes to skip in source before starting xfer
 *
 * On Exit (return == 0) success:
 *   sthp->args.ms.msglen
 *      Number of bytes accually transfered
 *
 * On Exit (return != 0) Failed:
 *      XFER_SRC_FAULT   Fault in source side
 *      XFER_DST_FAULT   Fault in destination side
 *
 * On Exit (preempted) restarted:
 *   dthp->restart
 *      Destination thread, has pointer to source thread
 *      so it can verify the message pass can continue.
 *
 *   sthp->args.ms.msglen
 *      Number of bytes that have been sucessfully transfered
 */

#define XFER_FAULTRET(fault)                      \
    SET_XFER_HANDLER(0);                          \
    get_active_thread(get_cpunum())->restart = 0; \
    return fault

/* xfer_skip() went past every entry of the vector. */
#define XFER_SKIP_ALL (-1)

/* Step *iovp forward over the *offp bytes still to be skipped, an entry at a
 * time, leaving *offp the offset into the entry it stops at. A bounded transfer
 * (sp set) stops with XFER_YIELD after XFER_STEP_ENTRIES entries, the position
 * left in place. Each entry's length is read once, under the fault handlers the
 * caller armed, so another thread cannot change it between the test and the
 * subtraction.
 *
 * Returns 0, XFER_SKIP_ALL, XFER_YIELD or a fault.
 */
static int xfer_skip(iov_t **iovp,
                     int *partsp,
                     size_t *offp,
                     struct xfer_spent *sp)
{
    jmp_buf env, *old_env = GET_XFER_ENV();
    iov_t *iov = *iovp;
    int parts = *partsp, ret = 0, status;
    size_t off = *offp;

    SET_XFER_ENV(&env);
    if ((status = xfer_setjmp(*GET_XFER_ENV()))) {
        SET_XFER_ENV(old_env);
        return status;
    }
    for (;;) {
        size_t len = GETIOVLEN(iov);

        if (off < len)
            break;
        off -= (unsigned) len;
        if (--parts == 0) {
            ret = XFER_SKIP_ALL;
            break;
        }
        ++iov;
        if (sp && ++sp->ents >= XFER_STEP_ENTRIES) {
            ret = XFER_YIELD;
            break;
        }
    }
    SET_XFER_ENV(old_env);
    *iovp = iov;
    *partsp = parts;
    *offp = off;
    return ret;
}

/* Save where a bounded transfer stands and arrange for the kernel call to run
 * again. It does not exit: xfermsg_run returns XFER_RESTART through the call,
 * which gives up the peer it claimed under the channel lock and returns to the
 * trap handler, which closes the rcvid lookup window and leaves the kernel.
 * Each side is the entry index into its vector and the offset to move forward
 * from there, so the restart resumes in place instead of skipping msglen bytes
 * again.
 */
static int xfer_yield(thread_t *act,
                      thread_t *dthp,
                      thread_t *sthp,
                      const iov_t *sbase,
                      const iov_t *src,
                      size_t soff,
                      const iov_t *dbase,
                      const iov_t *dst,
                      size_t doff)
{
    act->xcur.msglen = sthp->args.ms.msglen;
    act->xcur.sidx = (uint32_t) (src - sbase);
    act->xcur.soff = soff;
    act->xcur.didx = (uint32_t) (dst - dbase);
    act->xcur.doff = doff;
    act->xcur.armed = true;
    SET_XFER_HANDLER(0);
    KCALL_RESTART(act);
    return XFER_RESTART;
}

/* The restart point of every loop of a transfer: a preemption check for the
 * callers that restart by msglen, and for a bounded transfer a step of work
 * done or a preemption request, either of which saves the cursor and restarts.
 * It names the locals of xfermsg_run.
 */
#define XFER_CHECKPOINT(src, soff, dst, doff)                                \
    do {                                                                     \
        if (!bounded) {                                                      \
            XFER_PREEMPT(act);                                               \
        } else if (spent.ents >= XFER_STEP_ENTRIES ||                        \
                   spent.bytes >= XFER_STEP_BYTES || NEED_PREEMPT(act)) {    \
            return xfer_yield(act, dthp, sthp, sbase, src, soff, dbase, dst, \
                              doff);                                         \
        }                                                                    \
    } while (0)

/* Skip the bytes still to be skipped at the start of one vector. */
#define XFER_SKIP_SIDE(iov, parts, off)                                      \
    do {                                                                     \
        status = xfer_skip(&(iov), &(parts), &(off), sp);                    \
        if (status == XFER_SKIP_ALL) {                                       \
            SET_XFER_HANDLER(0);                                             \
            return 0;                                                        \
        }                                                                    \
        if (status == XFER_YIELD) {                                          \
            return xfer_yield(act, dthp, sthp, sbase, src, soff, dbase, dst, \
                              doff);                                         \
        }                                                                    \
        if (status) {                                                        \
            XFER_FAULTRET(status);                                           \
        }                                                                    \
    } while (0)

/* The boundary a bounded vector's entries must lie inside: that of the process
 * the vector belongs to, or 0 where entries are not checked (roottask, a kernel
 * buffer, or a transfer that is not bounded).
 */
static uintptr_t xfer_bound(bool bounded, unsigned flags, process_t *prp)
{
    if (!bounded || (flags & (MAPADDR_FLAGS_SYSPRP | MAPADDR_FLAGS_IOVKERNEL)))
        return 0;
    return prp->boundary_addr;
}

/* A bounded transfer reads the entries of a vector in this address space
 * itself, so the array must lie inside the boundary too (bound 0: not checked).
 */
static bool xfer_array_in_bound(const iov_t *iov,
                                unsigned parts,
                                uintptr_t bound)
{
    uintptr_t base = (uintptr_t) iov, last;

    if (!bound)
        return true;
    last = base + (size_t) parts * sizeof(iov_t) - 1;
    return base <= last && WITHIN_BOUNDARY(base, last, bound);
}

static int xfermsg_run(thread_t *dthp,
                       thread_t *sthp,
                       size_t doff,
                       size_t soff,
                       bool bounded)
{
    iov_t *dst, *src, *dbase, *sbase;
    int dparts, sparts;
    process_t *prp, *sprp, *dprp;
    unsigned sflags, dflags;
    int status;
    iov_t diov[1], siov[1];
    thread_t *act = get_active_thread(get_cpunum());
    struct xfer_spent spent = {0, 0};
    struct xfer_spent *sp = bounded ? &spent : NULL;
    bool resume = false, fresh;
    unsigned sall, dall;
    uintptr_t sbound, dbound;

    /* A bounded transfer restarts only at its own checkpoints, which save the
     * cursor and leave through the call, so the claim on the peer is given up
     * under the channel lock; XFER_PREEMPT would exit with it held.
     */
    if (!bounded) {
        XFER_PREEMPT(act);
    }

    /* If restarted, and threads match, continue where we left off. A bounded
     * transfer continues only from its own cursor, for the same vectors and
     * offsets: a call that finds some other cursor, or none, starts over.
     */
    fresh = dthp->restart != sthp || sthp->restart != sthp ||
            sthp->args.ms.dthp != dthp;
    if (bounded && !fresh)
        fresh = !(act->xcur.armed && act->xcur.sthp == sthp &&
                  act->xcur.dthp == dthp &&
                  act->xcur.msglen == sthp->args.ms.msglen &&
                  act->xcur.smsg == sthp->args.ms.smsg &&
                  act->xcur.sparts == (int32_t) sthp->args.ms.sparts &&
                  act->xcur.rmsg == dthp->args.ms.rmsg &&
                  act->xcur.rparts == (int32_t) dthp->args.ms.rparts &&
                  act->xcur.soff0 == soff && act->xcur.doff0 == doff);
    if (fresh) {
        sthp->args.ms.msglen = 0;
        sthp->args.ms.dthp = dthp;
        if (bounded) {
            act->xcur.armed = false;
            act->xcur.sthp = sthp;
            act->xcur.dthp = dthp;
            act->xcur.smsg = sthp->args.ms.smsg;
            act->xcur.sparts = (int32_t) sthp->args.ms.sparts;
            act->xcur.rmsg = dthp->args.ms.rmsg;
            act->xcur.rparts = (int32_t) dthp->args.ms.rparts;
            act->xcur.soff0 = soff;
            act->xcur.doff0 = doff;
        }

        /* CAREFUL! The above operations have to happen before dthp->restart
         * gets updated here. The compiler can rearrange code, and if it
         * rearranges the assignment to dthp->restart so that it comes before
         * the above, a preemption at a bad time can lead to failure in the
         * xfer. We need to keep the compiler from rearranging things here,
         * hence we use atomic_order().
         */
        atomic_order(sthp->restart = dthp->restart = sthp);
    } else if (bounded) {
        resume = true;
    } else {
        soff += sthp->args.ms.msglen;
        doff += sthp->args.ms.msglen;
    }

    /* Special case for 1 part source messages. */
    if ((sparts = (int) sthp->args.ms.sparts) == 0) {
        return 0;
    }

    if (sparts < 0) {
        SETIOV(siov, sthp->args.ms.smsg, PARTS_TO_LEN(sparts));
        src = siov;
        sparts = 1;
        sflags = MAPADDR_FLAGS_IOVKERNEL;
    } else {
        if (sparts > XFER_IOV_LIMIT) {
            return XFER_SRC_FAULT;
        }
        src = sthp->args.ms.smsg;
        sflags = 0;
    }

    if (sthp->process->pid == ROOTTASK_PID) {
        sflags |= MAPADDR_FLAGS_SYSPRP;
    }
    sprp = sthp->as_prp;

    /* Same special case for 1 part dst messages */
    if ((dparts = dthp->args.ms.rparts) == 0) {
        return 0;
    }

    if (dparts < 0) {
        SETIOV(diov, dthp->args.ms.rmsg, PARTS_TO_LEN(dparts));
        dst = diov;
        dparts = 1;
        dflags = MAPADDR_FLAGS_WRITE | MAPADDR_FLAGS_IOVKERNEL;
    } else {
        if (dparts > XFER_IOV_LIMIT) {
            return XFER_DST_FAULT;
        }
        dst = dthp->args.ms.rmsg;
        dflags = MAPADDR_FLAGS_WRITE;
    }

    if (dthp->process->pid == ROOTTASK_PID) {
        dflags |= MAPADDR_FLAGS_SYSPRP;
    }
    dprp = dthp->as_prp;

    if (bounded) {
        sflags |= MAPADDR_FLAGS_STEP;
        dflags |= MAPADDR_FLAGS_STEP;
    }

    sbase = src;
    dbase = dst;
    sall = (unsigned) sparts;
    dall = (unsigned) dparts;
    if (resume) {
        if (act->xcur.sidx >= sall) {
            return XFER_SRC_FAULT;
        }
        if (act->xcur.didx >= dall) {
            return XFER_DST_FAULT;
        }
        src += act->xcur.sidx;
        sparts -= (int) act->xcur.sidx;
        soff = act->xcur.soff;
        dst += act->xcur.didx;
        dparts -= (int) act->xcur.didx;
        doff = act->xcur.doff;
    }

    prp = pcpu_array[get_cpunum()].pc_as_prp;

    /* Server/loader threads are in pid 1 (roottask) and use process_bind to set
     * as_prp when they need cross-as access. In RTMux, threads can run directly
     * in child processes (e.g. loader_load via thread_create_r), so as_prp is
     * NULL even though the thread's process differs from the currently loaded
     * address space.
     *
     * Detect this and set sprp/dprp to the thread's process so the cross-as
     * transfer logic kicks in.
     */
    if (!sprp && sthp->process != prp) {
        sprp = sthp->process;
    }
    if (!dprp && dthp->process != prp) {
        dprp = dthp->process;
    }

    /* A bounded transfer checks each entry of the vectors it reads in this
     * address space against that vector's process, at the time it copies. The
     * other side of a cross-as transfer is checked by map_xfer.
     */
    sbound = xfer_bound(bounded, sflags, sprp ? sprp : sthp->process);
    dbound = xfer_bound(bounded, dflags, dprp ? dprp : dthp->process);
    if (!xfer_array_in_bound(sbase, sall, sbound)) {
        return XFER_SRC_FAULT;
    }
    if (!xfer_array_in_bound(dbase, dall, dbound)) {
        return XFER_DST_FAULT;
    }

    /* Find the translation or change address spaces as necessary. */
    /* dst_as src_as as <=> as
     *    1            2       !(1|2)      1 could not happen (for pipe case,
     * prepare the as in read_writev) 1 NULL !1 1 1 2 2 2(NC)* ++ 1 1 1 1(NC) 1
     * 2 1 1(NC)* 1 NULL 1 1(NC) NULL NULL any same(NC) NULL 2 2 2(NC) NULL 2 !2
     * 2
     */
    if (dprp) {
        if (sprp == prp) {
            if (dprp != prp) {
                iov_t iovlist[XFER_IOV_NUM];
                iov_t *dst0;
                size_t doff0;
                uint32_t msglen0;
                int parts;
                int nbytes;
                struct xfer_walk w;

                /* Check for a src offset and skip over it. */
                if (soff) {
                    SET_XFER_HANDLER(&xfer_src_handlers);
                    XFER_SKIP_SIDE(src, sparts, soff);
                }

                do {
                    XFER_CHECKPOINT(src, soff, dst, doff);
                    SET_XFER_HANDLER(&xfer_dst_handlers);
                    parts = sizeof iovlist / sizeof *iovlist;
                    dst0 = dst;
                    doff0 = doff;
                    msglen0 = sthp->args.ms.msglen;
                    nbytes =
                        vm_ops->map_xfer(prp, dprp, (iov_t **) &dst, &dparts,
                                         &doff, iovlist, &parts, dflags);
                    if (nbytes <= 0) {
                        if (nbytes == 0) {
                            break;
                        } else if (nbytes == XFER_MAP_YIELD) {
                            return xfer_yield(act, dthp, sthp, sbase, src, soff,
                                              dbase, dst, doff);
                        } else {
                            XFER_FAULTRET(XFER_DST_FAULT);
                        }
                    }
                    dflags |= MAPADDR_FLAGS_NOTFIRST;
                    spent.ents += (unsigned) parts;

                    SET_XFER_HANDLER(&xfer_src_handlers);
                    w = (struct xfer_walk){iovlist, src, parts,  sparts, 0,
                                           soff,    0,   sbound, sp};
                    if ((status = xferiov(sthp, &w)) != 0) {
                        if (status == XFER_YIELD) {
                            return xfer_yield(
                                act, dthp, sthp, sbase, w.src, w.soff, dbase,
                                dst0, doff0 + (sthp->args.ms.msglen - msglen0));
                        }
                        XFER_FAULTRET(status);
                    }
                    if (dparts == 0) {
                        break;
                    }
                    src = w.src;
                    sparts = w.sparts;
                    soff = w.soff;
                } while (sparts);
                SET_XFER_HANDLER(0);
                return 0;
            } else { /* end for (dprp != prp) */
                /* sprp == dprp == prp */
            }
        } else if (sprp) { /* end for (sprp == prp) */
            iov_t iovlist[XFER_IOV_NUM];
            iov_t *src0;
            size_t soff0;
            uint32_t msglen0;
            int parts;
            int nbytes;
            struct xfer_walk w;

            /* Check for a dst offset and skip over it. */
            if (doff) {
                SET_XFER_HANDLER(&xfer_dst_handlers);
                XFER_SKIP_SIDE(dst, dparts, doff);
            }

            do {
                XFER_CHECKPOINT(src, soff, dst, doff);
                SET_XFER_HANDLER(&xfer_src_handlers);

                parts = sizeof iovlist / sizeof *iovlist;
                src0 = src;
                soff0 = soff;
                msglen0 = sthp->args.ms.msglen;
                nbytes = vm_ops->map_xfer(prp, sprp, (iov_t **) &src, &sparts,
                                          &soff, iovlist, &parts, sflags);

                if (nbytes <= 0) {
                    if (nbytes == 0) {
                        break;
                    } else if (nbytes == XFER_MAP_YIELD) {
                        return xfer_yield(act, dthp, sthp, sbase, src, soff,
                                          dbase, dst, doff);
                    } else {
                        XFER_FAULTRET(XFER_SRC_FAULT);
                    }
                }
                sflags |= MAPADDR_FLAGS_NOTFIRST;
                spent.ents += (unsigned) parts;

                w = (struct xfer_walk){dst, iovlist, dparts, parts, doff,
                                       0,   dbound,  0,      sp};
                if ((status = xferiov(sthp, &w)) != 0) {
                    if (status == XFER_YIELD) {
                        return xfer_yield(
                            act, dthp, sthp, sbase, src0,
                            soff0 + (sthp->args.ms.msglen - msglen0), dbase,
                            w.dst, w.doff);
                    }
                    XFER_FAULTRET(status);
                }
                if (sparts == 0) {
                    break;
                }
                dst = w.dst;
                dparts = w.dparts;
                doff = w.doff;

            } while (dparts);
            SET_XFER_HANDLER(0);
            return 0;
        } else { /* end for (sprp) */
            if (dprp != prp) {
                vm_ops->as(dprp, &pcpu_array[get_cpunum()].pc_as_prp);
                XFER_CHECKPOINT(src, soff, dst, doff);
            }
        }
    } else {
        if (sprp != prp) {
            if (sprp) {
                vm_ops->as(sprp, &pcpu_array[get_cpunum()].pc_as_prp);
                XFER_CHECKPOINT(src, soff, dst, doff);
            }
        }
    }


    /* Check for a dst offset and skip over it. */
    if (doff) {
        SET_XFER_HANDLER(&xfer_dst_handlers);
        XFER_SKIP_SIDE(dst, dparts, doff);
    }

    SET_XFER_HANDLER(&xfer_src_handlers);

    /* Check for a src offset and skip over it. */
    if (soff) {
        XFER_SKIP_SIDE(src, sparts, soff);
    }

    {
        struct xfer_walk w = {dst,  src,    dparts, sparts, doff,
                              soff, dbound, sbound, sp};

        status = xferiov(sthp, &w);
        if (status == XFER_YIELD) {
            return xfer_yield(act, dthp, sthp, sbase, w.src, w.soff, dbase,
                              w.dst, w.doff);
        }
    }

    SET_XFER_HANDLER(0);
    return status;
}

int xfermsg(thread_t *dthp, thread_t *sthp, size_t doff, size_t soff)
{
    return xfermsg_run(dthp, sthp, doff, soff, false);
}

/* xfermsg for msg_readv and msg_writev: each kernel entry does at most a step
 * of work (XFER_STEP_ENTRIES vector entries stepped over, XFER_STEP_BYTES
 * copied), then saves where it stands and restarts the call. The vector entries
 * it uses are read once and checked as it copies.
 */
int xfermsg_bounded(thread_t *dthp, thread_t *sthp, size_t doff, size_t soff)
{
    int status = xfermsg_run(dthp, sthp, doff, soff, true);

    /* A cursor is for the restart of a call that yielded, not for a later call
     * that happens to look the same.
     */
    if (status != XFER_RESTART) {
        thread_t *act = get_active_thread(get_cpunum());

        if (act->xcur.armed)
            act->xcur.armed = false;
    }
    return status;
}
