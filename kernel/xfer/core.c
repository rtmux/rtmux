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
#define XFER_IOV_LIMIT \
    ((4 * 1024 * 1024) / 8) /* Maximum number of iov allowed */

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
            if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
                atomic_clr((volatile unsigned *) &thp->internal_flags,
                           RTMUX_ITF_MSG_DELIVERY);
                if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
                    force_ready(thp, MSG_FORCE_ERROR(thp));
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
        CRASHCHECK(TYPE_MASK(thp->type) != TYPE_THREAD);
        SETKIP(thp, KIP(thp) - KSYS_ENTRY_SIZE);
    }
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    xfer_restorestate(thp);
#endif
    SET_XFER_HANDLER(NULL);
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

#define BOUNDARY_CHECK(bound, iov, parts, fault)          \
    if (xfer_memchk(bound, iov, (size_t) (parts)) != 0) { \
        XFER_FAULTRET(fault);                             \
    }

#define OFFSET_SKIP(off, iov, iovparts)   \
    while (off >= GETIOVLEN(iov)) {       \
        off -= (unsigned) GETIOVLEN(iov); \
        if (--iovparts == 0) {            \
            SET_XFER_HANDLER(0);          \
            return 0;                     \
        }                                 \
        ++iov;                            \
    }

#define OFFSET_SKIP_BR(off, iov, iovparts)    \
    do {                                      \
        while (off >= GETIOVLEN(iov)) {       \
            off -= (unsigned) GETIOVLEN(iov); \
            if (--iovparts == 0)              \
                break;                        \
            ++iov;                            \
        }                                     \
        SET_XFER_HANDLER(0);                  \
    } while (0)


int xfermsg(thread_t *dthp, thread_t *sthp, unsigned doff, unsigned soff)
{
    iov_t *dst, *src;
    int dparts, sparts;
    process_t *prp, *sprp, *dprp;
    unsigned sflags, dflags;
    int status;
    iov_t diov[1], siov[1];

    XFER_PREEMPT(get_active_thread(get_cpunum()));

    /* If restarted, and threads match, continue where we left off */
    if (dthp->restart != sthp || sthp->restart != sthp ||
        sthp->args.ms.dthp != dthp) {
        sthp->args.ms.msglen = 0;
        sthp->args.ms.dthp = dthp;

        /* CAREFUL! The above operations have to happen before dthp->restart
         * gets updated here. The compiler can rearrange code, and if it
         * rearranges the assignment to dthp->restart so that it comes before
         * the above, a preemption at a bad time can lead to failure in the
         * xfer. We need to keep the compiler from rearranging things here,
         * hence we use atomic_order().
         */
        atomic_order(sthp->restart = dthp->restart = sthp);
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
                int parts;
                int nbytes;

                /* Check for a src offset and skip over it. */
                if (soff) {
                    OFFSET_SKIP(soff, src, sparts);
                }

                do {
                    XFER_PREEMPT(get_active_thread(get_cpunum()));
                    SET_XFER_HANDLER(&xfer_dst_handlers);
                    parts = sizeof iovlist / sizeof *iovlist;
                    nbytes = vm_ops->map_xfer(prp, dprp, (iov_t **) &dst,
                                              &dparts, (int *) &doff, iovlist,
                                              &parts, dflags);
                    if (nbytes <= 0) {
                        if (nbytes == 0) {
                            break;
                        } else {
                            XFER_FAULTRET(XFER_DST_FAULT);
                        }
                    }
                    dflags |= MAPADDR_FLAGS_NOTFIRST;

                    SET_XFER_HANDLER(&xfer_src_handlers);
                    if ((status = xferiov(sthp, iovlist, src, parts, sparts, 0,
                                          soff)) != 0) {
                        XFER_FAULTRET(status);
                    }
                    if (dparts == 0) {
                        break;
                    }
                    soff += (unsigned) nbytes;
                    OFFSET_SKIP_BR(soff, src, sparts);
                } while (sparts);
                SET_XFER_HANDLER(0);
                return 0;
            } else { /* end for (dprp != prp) */
                /* sprp == dprp == prp */
            }
        } else if (sprp) { /* end for (sprp == prp) */
            iov_t iovlist[XFER_IOV_NUM];
            int parts;
            int nbytes;


            /* Check for a dst offset and skip over it. */
            if (doff) {
                OFFSET_SKIP(doff, dst, dparts);
            }


            do {
                XFER_PREEMPT(get_active_thread(get_cpunum()));
                SET_XFER_HANDLER(&xfer_src_handlers);

                parts = sizeof iovlist / sizeof *iovlist;
                nbytes =
                    vm_ops->map_xfer(prp, sprp, (iov_t **) &src, &sparts,
                                     (int *) &soff, iovlist, &parts, sflags);

                if (nbytes <= 0) {
                    if (nbytes == 0) {
                        break;
                    } else {
                        XFER_FAULTRET(XFER_SRC_FAULT);
                    }
                }
                sflags |= MAPADDR_FLAGS_NOTFIRST;

                if ((status = xferiov(sthp, dst, iovlist, dparts, parts, doff,
                                      0)) != 0) {
                    XFER_FAULTRET(status);
                }
                if (sparts == 0) {
                    break;
                }
                doff += (unsigned) nbytes;
                OFFSET_SKIP_BR(doff, dst, dparts);

            } while (dparts);
            SET_XFER_HANDLER(0);
            return 0;
        } else { /* end for (sprp) */
            if (dprp != prp) {
                vm_ops->as(dprp, &pcpu_array[get_cpunum()].pc_as_prp);
                XFER_PREEMPT(get_active_thread(get_cpunum()));
            }
        }
    } else {
        if (sprp != prp) {
            if (sprp) {
                vm_ops->as(sprp, &pcpu_array[get_cpunum()].pc_as_prp);
                XFER_PREEMPT(get_active_thread(get_cpunum()));
            }
        }
    }


    /* Check for a dst offset and skip over it. */
    if (doff) {
        SET_XFER_HANDLER(&xfer_dst_handlers);
        OFFSET_SKIP(doff, dst, dparts);
    }

    SET_XFER_HANDLER(&xfer_src_handlers);

    /* Check for a src offset and skip over it. */
    if (soff) {
        OFFSET_SKIP(soff, src, sparts);
    }

    status = xferiov(sthp, dst, src, dparts, sparts, doff, soff);


    SET_XFER_HANDLER(0);
    return status;
}

/* for aysnc msg receive */
int rcvmsg(thread_t *dthp,
           process_t *sprp,
           void *destp,
           int destparts,
           void *srcp,
           int srcparts)
{
    iov_t *dst, *src;
    int sparts, dparts;
    process_t *dprp;
    unsigned sflags;
    int status;
    iov_t diov[1], siov[1];
    unsigned soff, doff;

    /* restart check */
    soff = doff = dthp->args.ms.msglen;

    if (srcparts == 0 || destparts == 0) {
        return 0;
    }

    /* Special case for 1 part source messages. */
    if (srcparts < 0) {
        SETIOV(siov, srcp, PARTS_TO_LEN(srcparts));
        src = siov;
        sparts = 1;
        sflags = MAPADDR_FLAGS_IOVKERNEL;
    } else {
        if (srcparts > XFER_IOV_LIMIT) {
            return XFER_SRC_FAULT;
        }
        src = srcp;
        sparts = srcparts;
        sflags = 0;
    }

    /* Same special case for 1 part dst messages */
    if (destparts < 0) {
        SETIOV(diov, destp, PARTS_TO_LEN(destparts));
        dst = diov;
        dparts = 1;
    } else {
        if (destparts > XFER_IOV_LIMIT) {
            return XFER_DST_FAULT;
        }
        dst = destp;
        dparts = destparts;
    }

    dprp = dthp->as_prp;

    /* act is receiver */
    if (dprp != sprp) {
        iov_t iovlist[XFER_IOV_NUM];
        int parts = 0;
        int nbytes;

        /* check dst iov addresses */
        SET_XFER_HANDLER(&xfer_dst_handlers);
        if (dprp->pid != ROOTTASK_PID) {
            BOUNDARY_CHECK(dprp->boundary_addr, dst, dparts, XFER_DST_FAULT);
        }

        /* Check for a dst offset and skip over it. */
        if (doff) {
            OFFSET_SKIP(doff, dst, dparts);
        }

        do {
            XFER_PREEMPT(get_active_thread(get_cpunum()));
            SET_XFER_HANDLER(&xfer_async_handlers);

            if (sprp->pid != ROOTTASK_PID) {
                BOUNDARY_CHECK(sprp->boundary_addr, src, sparts,
                               XFER_DST_FAULT);
            }

            parts = sizeof iovlist / sizeof *iovlist;
            nbytes = vm_ops->map_xfer(dprp, sprp, (iov_t **) &src, &sparts,
                                      (int *) &soff, iovlist, &parts, sflags);
            if (nbytes <= 0) {
                if (nbytes == 0) {
                    break;
                } else {
                    XFER_FAULTRET(XFER_SRC_FAULT);
                }
            }
            sflags |= MAPADDR_FLAGS_NOTFIRST;

            if ((status = xferiov(dthp, dst, iovlist, dparts, parts, doff,
                                  soff)) != 0) {
                XFER_FAULTRET(status);
            }
            if (sparts == 0) {
                break;
            }
            doff += (unsigned) nbytes;
            OFFSET_SKIP_BR(doff, dst, dparts);

        } while (dparts);
        SET_XFER_HANDLER(0);
    } else {
        /* dprp == sprp */

        /* check dst iov addresses */
        if (dprp->pid != ROOTTASK_PID) {
            SET_XFER_HANDLER(&xfer_dst_handlers);
            BOUNDARY_CHECK(dprp->boundary_addr, dst, dparts, XFER_DST_FAULT);
        }

        /* Check for a dst offset and skip over it. */
        if (doff) {
            SET_XFER_HANDLER(&xfer_dst_handlers);
            OFFSET_SKIP(doff, dst, dparts);
        }

        if (sprp->pid != ROOTTASK_PID) {
            SET_XFER_HANDLER(&xfer_src_handlers);
            BOUNDARY_CHECK(sprp->boundary_addr, src, sparts, XFER_DST_FAULT);
        }

        /* Check for a dst offset and skip over it. */
        if (soff) {
            SET_XFER_HANDLER(&xfer_src_handlers);
            OFFSET_SKIP(soff, src, sparts);
        }

        /* xfer */
        SET_XFER_HANDLER(&xfer_async_handlers);
        status = xferiov(dthp, dst, src, dparts, sparts, doff, soff);
        SET_XFER_HANDLER(0);
    }

    return EOK;
}
