/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <core/globals.h>

/* Isolate the lowest set bit (two's complement; C17 mandates this) */
#define GET_BOTTOM_BIT(v) (((v) & ((v) - 1)) ^ (v))

/* Run a thread's pending special-return actions.
 *
 * Drains the thread's RTMUX_FLG_THR_SPECRET_MASK flags one bit at a time,
 * performing the deferred work that must run with user-memory access available
 * and fault recovery enabled: receive-info / short-message / notification data
 * transfer, nanosleep/join result copy-out, thread startup (WAAA), mutex
 * acquisition and ceiling changes, cancellation and self-destruction. Each
 * iteration may fault on user memory and be preempted, so the loop re-checks
 * the active thread and specret state. Parameters:
 *   thp : thread whose special-return work is processed
 */
void specialret_attr specialret(thread_t *thp)
{
    int flags;

    /* The following code may fault since it accesses user memory. If the global
     * inspecret is set then the fault handler takes special action. It
     * overrides the INKERNEL_LOCKED flag and allows a page to be allocated or
     * brought in (dynamic stack growth/paging) or if a true fault then the
     * special return flag in the thread which caused the fault is cleared and a
     * signal is set on the thread.
     */
#ifndef NDEBUG
    /* The specret-active flag is per-CPU (pc_in_specret). */
    if (pcpu_array[get_cpunum()].pc_in_specret)
        crash();
#endif
    specret_kernel();

    while ((flags = (thp->flags & RTMUX_FLG_THR_SPECRET_MASK))) {
        SPECRET_PREEMPT(thp);
        switch ((inspecret = GET_BOTTOM_BIT(flags))) {
        case RTMUX_FLG_THR_TO_BE_STOPPED: {
            atomic_clr((volatile unsigned *) &thp->flags,
                       RTMUX_FLG_THR_TO_BE_STOPPED);
            thp->state = STATE_STOPPED;
            _TRACE_TH_EMIT_STATE(thp, STOPPED);
            block();
            break;
        }

        case RTMUX_FLG_THR_RCVINFO: {
            struct _msg_info *mep = thp->args.ri.info;
            connect_t *cop = thp->args.ri.cop;
            thread_t *thp2 = thp->args.ri.thp;

            if (cop->channel->flags & RTMUX_FLG_CHF_GLOBAL) {
                mep->priority = (int16_t) thp->args.ri.value;
                mep->msglen = (uint32_t) thp->args.ri.id;
            } else {
                get_rcvinfo(thp2, -1, cop, mep);
                if ((thp->flags & RTMUX_FLG_THR_SHORT_MSG) == 0) {
                    /* If there is no short message, remove the connection of
                     * the client thread to the server thread.
                     */
                    thp2->restart = 0;

                    /* Clear the flag that indicates we (the client) need data
                     * from thp2's (the sender's) control block.
                     */
                    atomic_clr((volatile unsigned *) &thp2->internal_flags,
                               RTMUX_ITF_SPECRET_PENDING);
                } else {
                    if ((uint32_t) xferlen(thp, thp->args.ri.rmsg,
                                           thp->args.ri.rparts) < mep->msglen) {
                        mep->msglen = (uint32_t) xferlen(thp, thp->args.ri.rmsg,
                                                         thp->args.ri.rparts);
                    }
                }
            }
            atomic_clr((volatile unsigned *) &thp->flags,
                       RTMUX_FLG_THR_RCVINFO);

            break;
        }

        case RTMUX_FLG_THR_SHORT_MSG: {
            int status = 0;
            thread_t *thp2 = thp->blocked_on;

#ifndef NDEBUG

            /* We (thp) are receiving a short message. If thp==thp2, then this
             * is a msg_reply. If thp!=thp2 then this is a msg_send and thp2 is
             * the sender. thp2 better be in state reply-blocked when we (the
             * receiver) start to run.
             */
            if (thp != thp2 && thp2->state != STATE_REPLY &&
                thp2->state != STATE_NET_REPLY)
                crash();
#endif

            status =
                xfer_cpy_diov(thp, thp->args.ri.rmsg, thp2->args.msbuff.buff,
                              thp->args.ri.rparts, thp2->args.msbuff.msglen);

            atomic_clr((volatile unsigned *) &thp->flags,
                       RTMUX_FLG_THR_SHORT_MSG | RTMUX_FLG_THR_BUFF_MSG);

            /* Clear the flag that indicates we (the client) need data from
             * thp2's (the sender's) control block.
             */
            if (thp != thp2) {
                atomic_clr((volatile unsigned *) &thp2->internal_flags,
                           RTMUX_ITF_SPECRET_PENDING);
            }

            /* Since RTMUX_FLG_THR_RCVINFO is higher priority, we can always
             * clear the client connection to the server thread.
             */
            thp2->restart = 0;

            if (status) {
                kcall_err(thp, EFAULT);
                if (thp != thp2) {
                    /* thp is the server and thp2 is the client, this is a
                     * server fault during receiving
                     */
                    force_ready(thp2, EFAULT);
                }
            }
            break;
        }

        case RTMUX_FLG_THR_NOTIFICATION: {
            int status;

            status =
                xfernotification(thp, thp->args.ri.rmsg, thp->args.ri.rparts,
                                 (uint32_t) thp->args.ri.code,
                                 thp->args.ri.value, (int32_t) thp->args.ri.id);

            atomic_clr((volatile unsigned *) &thp->flags,
                       RTMUX_FLG_THR_NOTIFICATION);
            if (status) {
                kcall_err(thp, EFAULT);
            }

            break;
        }

        /* Both writebacks below go through xfer_memcpy rather than a raw
         * store. The pointers were checked and probed by the kcall that parked
         * them here, but that was arbitrarily long ago (the thread has since
         * blocked and been woken), so the caller has had every opportunity to
         * unmap the page. This path arms no fault handler of its own, so a raw
         * store to a now-unmapped address was an unrecoverable S-mode fault
         * from an unprivileged caller. xfer_memcpy is also what avoids the
         * floating-point register traffic a plain struct assignment can emit,
         * which is why the nanosleep arm already used memcpy.
         *
         * A failed writeback reports EFAULT to the woken thread and still
         * clears the flag: the sleep or join itself completed, and leaving the
         * flag set would re-run this arm forever. Same shape as the
         * NOTIFICATION arm above.
         */
        case RTMUX_FLG_THR_NANOSLEEP: {
            int status = 0;

            if (thp->args.to.timeptr) {
                status = xfer_memcpy(thp->args.to.timeptr,
                                     &thp->args.to.left.nsec,
                                     sizeof(*thp->args.to.timeptr));
            }

            atomic_clr((volatile unsigned *) &thp->flags,
                       RTMUX_FLG_THR_NANOSLEEP);
            if (status) {
                kcall_err(thp, EFAULT);
            }
            break;
        }

        case RTMUX_FLG_THR_JOIN: {
            int status = 0;

            if (thp->args.jo.statusptr) {
                status = xfer_memcpy(thp->args.jo.statusptr,
                                     &thp->args.jo.status,
                                     sizeof(thp->args.jo.status));
            }

            atomic_clr((volatile unsigned *) &thp->flags, RTMUX_FLG_THR_JOIN);
            if (status) {
                kcall_err(thp, EFAULT);
            }
            break;
        }

        case RTMUX_FLG_THR_WAAA:
            thread_specret(thp);
            break;

        case RTMUX_FLG_THR_ACQUIRE_MUTEX: {
            /* We need to save status since sync_mutex_lock() will probably set
             * it to zero. In the case of a condvar timeout we need to preserve
             * it.
             */
            int status = (int) KSTATUS(thp);
            sync_t *mutex = thp->args.mu.mutex;
            unsigned long want = SYNC_OWNER(thp);

            CRASHCHECK(!mutex);

            if ((mutex->owner & RTMUX_SYNC_OWNER_MASK) == want) {
                mutex->count += thp->args.mu.incr;
                atomic_clr((volatile unsigned *) &thp->flags,
                           RTMUX_FLG_THR_ACQUIRE_MUTEX);
            } else {
                sync_mutex_lock(thp, mutex, 0);

                if (thp->state == STATE_RUNNING) {
                    /* We got the mutex, update the count. Normally incr is
                     * zero, but it'll be one if this mutex is being used with a
                     * condvar wait.
                     */
                    mutex->count += thp->args.mu.incr;
                    atomic_clr((volatile unsigned *) &thp->flags,
                               RTMUX_FLG_THR_ACQUIRE_MUTEX);
                }
            }
            /* Timeout always overrides success on getting the mutex. */
            if (status == ETIMEDOUT) {
                SETKSTATUS(thp, status);
            }

            break;
        }

        case RTMUX_FLG_THR_CANCELSELF:
            atomic_or((volatile unsigned *) &thp->flags,
                      RTMUX_FLG_THR_KCALL_ERR_SET);
            SETKIP_FUNC(thp, thp->process->canstub);
            atomic_clr((volatile unsigned *) &thp->flags,
                       RTMUX_FLG_THR_CANCELSELF);
            break;

        case RTMUX_FLG_THR_KILLSELF:
            atomic_clr((volatile unsigned *) &thp->flags,
                       RTMUX_FLG_THR_KILLSELF);
            atomic_or((volatile unsigned *) &thp->flags,
                      RTMUX_FLG_THR_DETACHED);
            thread_destroy(thp);
            break;

        default:
#ifndef NDEBUG
            crash();
#endif
            break;
        }
        if (thp != get_active_thread(get_cpunum()) ||
            pcpu_array[get_cpunum()].pc_in_specret == 0) {
            break;
        }
    }

    unspecret_kernel();
    thp->restart = 0;
}
