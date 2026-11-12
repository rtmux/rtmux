/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <trace/events.h>

/* Translate a public interrupt vector to an internal interrupt level.
 *
 * Handles synthetic hook vectors directly, otherwise walks the intrinfo tables
 * to map the vector to a level, following cascade controllers and rejecting
 * disallowed levels. Parameters:
 *   act    : active thread to receive an errno on failure (may be NULL)
 *   vector : public interrupt vector number
 * Return value: the internal level, or -1 on error.
 */
int rdecl get_interrupt_level(thread_t *act, unsigned vector)
{
    unsigned level;
    int cascade;
    unsigned i;
    struct intrinfo_entry *iip;

    if (vector >= RTMUX_INTR_CLASS_SYNTHETIC && vector <= RTMUX_HOOK_LAST) {
        /* A valid hook routine */
        return HOOK_TO_LEVEL(vector);
    }
    for (;;) {
        iip = intrinfoptr;
        level = 0;
        i = 0;
        for (;;) {
            if (i >= intrinfo_num) {
                /* out of range number */
                if (act)
                    kcall_err(act, EINVAL);
                return -1;
            }
            if (vector >= iip->vector_base &&
                vector < (iip->vector_base + iip->num_vectors))
                break;
            level += iip->num_vectors;
            ++i;
            ++iip;
        }
        level += vector - iip->vector_base;
        cascade = interrupt_level[level].cascade_level;
        if (cascade < 0) {
            if ((interrupt_level[level].config & INTR_CONFIG_FLAG_DISALLOWED) &&
                pcpu_array[0].pc_alive) {
                if (act)
                    kcall_err(act, EINVAL);
                return -1;
            }
            return (int) level;
        }

        /* Guy wanted a cascaded interrupt level. Give him the first one of the
         * secondary controller instead.
         */
        vector = interrupt_level[cascade].info->vector_base;
    }
}

/* No-op interrupt handler used for pre-attached levels. Parameters:
 *   dummy : unused handler area
 *   id    : unused interrupt id
 * Return value: always NULL.
 */
static const struct sigevent *dummy_handler(void *dummy, int id)
{
    return NULL;
}

/* The plus two is because we have to leave the "-1" slot open for error
 * reporting.
 */
#define NUM_HOOK_RTNS ((RTMUX_HOOK_LAST + 2) & ~RTMUX_INTR_CLASS_SYNTHETIC)

/* Build the interrupt-level table at boot.
 *
 * Allocates interrupt_level[] (with leading slots reserved for synthetic
 * hooks), initializes each level from its intrinfo entry, wires up the cascade
 * controllers, and pre-attaches the dummy handler to any level flagged
 * PREATTACH.
 */
void rdecl interrupt_init()
{
    unsigned i;
    struct intrinfo_entry *iip;
    unsigned short level_base;
    unsigned num_external_levels;
    intr_level_t *ilp;

    num_external_levels = 0;
    iip = intrinfoptr;
    for (i = 0; i < intrinfo_num; ++i, ++iip) {
        num_external_levels += iip->num_vectors;
    }
    interrupt_level = _scalloc((num_external_levels + NUM_HOOK_RTNS) *
                               sizeof(*interrupt_level));
    interrupt_level += NUM_HOOK_RTNS; /* Point at first external level */

    /* Set the mask count of all interrupt levels to one, point at appropriate
     * info entry.
     */
    level_base = 0;
    iip = intrinfoptr;
    for (i = 0; i < num_external_levels; ++i) {
        if (i >= (level_base + iip->num_vectors)) {
            level_base = (unsigned short) (level_base + iip->num_vectors);
            ++iip;
        }
        ilp = &interrupt_level[i];
        ilp->info = iip;
        ilp->cascade_level = -1;
        ilp->level_base = level_base;
        ilp->mask_count = 1;
        ilp->config =
            (unsigned short) (iip->config ? iip->config(_syspage_ptr, iip,
                                                        (int) (i - level_base))
                                          : 0);
    }
    /* Generate the interrupt handlers */
    cpu_interrupt_init(num_external_levels);

    /* Initialize the cascaded entries */
    level_base = 0;
    iip = intrinfoptr;
    for (i = 0; i < intrinfo_num; ++i, ++iip) {
        int level = get_interrupt_level(NULL, iip->cascade_vector);

        if (level != -1) {
            ilp = &interrupt_level[level];
            ilp->cascade_level = (signed short) level_base;
            (void) ilp->info->unmask(_syspage_ptr,
                                     (int) (level - ilp->level_base));
            ilp->mask_count = 0;
        }
        level_base = (unsigned short) (level_base + iip->num_vectors);
    }
    for (i = 0; i < num_external_levels; ++i) {
        if (interrupt_level[i].config & INTR_CONFIG_FLAG_PREATTACH) {
            (void) interrupt_attach(i, dummy_handler, NULL, 0);
        }
    }
}

/* interrupt_attach — attach a handler to an interrupt level.
 *
 * Two modes:
 *   kernel callback: kernel-internal ISR callback (e.g., clock_handler)
 *   no callback: IST model — the calling thread is the handler, woken from
 *                interrupt_wait() when the interrupt fires.
 */
int rdecl interrupt_attach(unsigned level,
                           const struct sigevent *(*handler)(void *area,
                                                             int id),
                           void *area,
                           unsigned flags)
{
    interrupt_t *itp;
    interrupt_t **owner;
    intr_level_t *ilp;
    unsigned count;
    thread_t *act = get_active_thread(get_cpunum());
    int id;

    count = 0;
    ilp = &interrupt_level[level];
    owner = &ilp->queue;
    for (;;) {
        itp = *owner;
        if (!itp)
            break;
        ++count;
        owner = &itp->next;
    }
    if (count > INTR_LEVEL_ATTACH_MAX) {
        kcall_err(act, EAGAIN);
        return -1;
    }
    if (!(flags & RTMUX_INTR_FLAGS_END)) {
        owner = &ilp->queue;
    }
    if (!(itp = object_alloc(act->process, &interrupt_souls))) {
        kcall_err(act, EAGAIN);
        return -1;
    }

    itp->thread = act;
    itp->level = level;
    itp->handler = handler;
    itp->area = area;
    itp->flags = flags;

    /* Classify the attach. interrupt_attach() is the shared entry point for the
     * THREAD path (no callback, used by interrupt_attach_thread) and the
     * HANDLER path (kernel callback — clock_handler / trace). The EVENT path
     * uses interrupt_attach_event() which sets kind=INTR_KIND_EVENT explicitly.
     * "event" is zero-initialized here so the trap handler can safely ignore it
     * for THREAD / HANDLER cases.
     */
    itp->kind = handler ? INTR_KIND_HANDLER : INTR_KIND_THREAD;
    memset(&itp->event, 0, sizeof(itp->event));

    /* Per-hart pinning: initialized to "no owner" here; the kcall_exit caller
     * (ksys_interrupt_attach_thread / ksys_interrupt_attach_event) records the
     * owning hart AFTER this returns but BEFORE it performs the explicit
     * __interrupt_unmask.
     */
    itp->owner_hart = 0xFF;
    cpu_intr_attach(itp, act);

    if ((id = vector_add(&interrupt_vector, itp, 0)) == -1) {
        object_free(act->process, &interrupt_souls, itp);
        kcall_err(act, EAGAIN);
        return -1;
    }
    itp->id = id;
    act->process->flags |= RTMUX_FLG_PROC_CHECK_INTR;

    itp->next = *owner;
    *owner = itp;

    /* Auto-unmask retained only for the kernel-handler path (clock_handler /
     * trace). IST attach without a callback must defer the unmask to its
     * kcall_exit caller so it lands AFTER the caller has recorded the
     * source-pinning owner hart — otherwise the unmask would SENABLE-broadcast
     * and defeat pinning.
     */
    if (count == 0 && handler && !(flags & RTMUX_INTR_FLAGS_NO_UNMASK)) {
        do {
        } while (interrupt_unmask(level, NULL) != 0);
    }
    return id;
}

/* interrupt_attach_event — attach a sigevent to an interrupt level.
 *
 * Companion to interrupt_attach() above (the direct threaded-IRQ wake model).
 * When the IRQ fires, the trap handler delivers a COPY of "*event" inline via
 * sigevent_deliver_inline(); there is no softirq/workqueue handoff. Supports
 * SIGEV_INTR, SIGEV_NOTIFICATION and SIGEV_UNBLOCK; SIGEV_THREAD is rejected at
 * attach time because thread_create is unsafe from trap context.
 *
 * The calling thread's process owns the attach (used for cleanup on process
 * exit, identical to interrupt_attach). Caller (the ksys_interrupt_attach_event
 * kernel extension) is responsible for permission checks and the level
 * translation from public vector to internal level.
 */
int rdecl interrupt_attach_event(unsigned level,
                                 const struct sigevent *event,
                                 unsigned flags)
{
    interrupt_t *itp;
    interrupt_t **owner;
    intr_level_t *ilp;
    unsigned count;
    thread_t *act = get_active_thread(get_cpunum());
    int id;

    if (!event) {
        kcall_err(act, EINVAL);
        return -1;
    }

    /* The direct-delivery model cannot run thread_create from trap context, so
     * reject SIGEV_THREAD at attach time.
     */
    if (SIGEV_GET_TYPE(event) == SIGEV_THREAD) {
        kcall_err(act, EINVAL);
        return -1;
    }

    count = 0;
    ilp = &interrupt_level[level];
    owner = &ilp->queue;
    for (;;) {
        itp = *owner;
        if (!itp)
            break;
        ++count;
        owner = &itp->next;
    }
    if (count > INTR_LEVEL_ATTACH_MAX) {
        kcall_err(act, EAGAIN);
        return -1;
    }
    if (!(flags & RTMUX_INTR_FLAGS_END)) {
        owner = &ilp->queue;
    }
    if (!(itp = object_alloc(act->process, &interrupt_souls))) {
        kcall_err(act, EAGAIN);
        return -1;
    }

    itp->thread = act;
    itp->level = level;
    itp->handler = NULL;
    itp->area = NULL;
    itp->flags = flags;
    itp->kind = INTR_KIND_EVENT;
    itp->event = *event;

    /* Reserve one notification soul so SIGEV_NOTIFICATION delivery from the
     * trap handler is allocation-free (§15). Only this event kind queues a
     * notification; SIGEV_INTR/SIGEV_UNBLOCK do not. Fail the attach rather
     * than risk a dropped wakeup under memory pressure later.
     */
    itp->nresv = 0;
    if (SIGEV_GET_TYPE(event) == SIGEV_NOTIFICATION) {
        if (!object_reserve(&notification_souls)) {
            object_free(act->process, &interrupt_souls, itp);
            kcall_err(act, EAGAIN);
            return -1;
        }
        itp->nresv = 1;
    }

    /* Per-hart pinning: kcall_exit caller (ksys_interrupt_attach_event) will
     * fill in the owner hart and then unmask explicitly.
     */
    itp->owner_hart = 0xFF;
    cpu_intr_attach(itp, act);

    if ((id = vector_add(&interrupt_vector, itp, 0)) == -1) {
        if (itp->nresv)
            object_unreserve(&notification_souls);
        object_free(act->process, &interrupt_souls, itp);
        kcall_err(act, EAGAIN);
        return -1;
    }
    itp->id = id;
    act->process->flags |= RTMUX_FLG_PROC_CHECK_INTR;

    itp->next = *owner;
    *owner = itp;

    /* Auto-unmask removed: caller does it explicitly after recording the
     * source-pinning owner hart. See ksys_interrupt_attach_event.
     */
    return id;
}

/* Detach an interrupt entry and release it.
 *
 * Removes the interrupt_t from interrupt_vector, masks the level (and clears
 * any per-hart PLIC owner) if it was the last handler, unlinks it from the
 * level queue, flushes in-flight IRQs, and frees the object (plus the
 * kernel-allocated sigevent for the event path).
 *
 * Mutates global interrupt state (interrupt_vector, the level queue and mask
 * counts, the PLIC mask) without a dedicated slock; attach/detach is
 * root/IOPRIV-only and driver-init-time, and the in-kernel-call preemption
 * fence bridges the multi-step mutation. The fence lives here rather than at
 * the call site because kext_exit's process-teardown path also reaches this
 * function. Parameters:
 *   prp   : owning process (used to bill the freed object)
 *   index : interrupt_vector slot to detach
 */
void rdecl interrupt_detach_entry(process_t *prp, unsigned index)
{
    interrupt_t *itp;

    /* Remove the entry from the interrupt vector. */
    if (!(itp = vector_rem(&interrupt_vector, index)))
        crash();

    /* If no more handlers, disable the interrupt. It should be done before
     * removing the itp from the interrupt table
     */
    if (!interrupt_level[itp->level].queue->next &&
        (interrupt_level[itp->level].queue == itp)) {
#ifdef CONFIG_INSTRUMENTATION
        if (HOOK_TO_LEVEL(RTMUX_HOOK_TRACE) == itp->level) {
            /* If a tracelogger daemon was attached and was not logging in
             * ringmode, then stop tracing.
             */
            if (!trace_masks.ring_mode) {
                trace_masks.main_flags = 0;
            }
        }
#endif
        (void) interrupt_mask(itp->level, NULL);
        interrupt_level[itp->level].mask_count = 1;
        itp->mask_count = 0;
        /* Per-hart pinning: drop the recorded owner so a subsequent re-attach
         * of this PLIC source starts from a clean slate. Weak-linked: on
         * non-PLIC arches the symbol is absent and the call is skipped.
         */
        {
            extern __attribute__((weak)) void plic_clear_owner_hart(int irq);
            if (plic_clear_owner_hart) {
                int irq = (int) itp->level -
                          (int) interrupt_level[itp->level].level_base;
                plic_clear_owner_hart(irq);
            }
        }
    } else {
        if (itp->flags & RTMUX_INTR_FLAGS_TRK_MSK) {
            /* If we've been keeping track of how many times an attach point has
             * masked a level, undo those masks now.
             */
            while (itp->mask_count != 0) {
                (void) interrupt_unmask(itp->level, itp);
            }
        }
    }

    /* Remove interrupt handler from the interrupt table. NOTE: We're assuming
     * that LINK1_REM is unlinking atomicly
     */
    LINK1_REM(interrupt_level[itp->level].queue, itp, interrupt_t);

    /* flush out the interrupt queues to make sure there's nothing pending
     * that's still pointing at the interrupt_t object
     */
    intrevent_flush();

    /* Release the notification-soul reservation this source took at attach. */
    if (itp->nresv)
        object_unreserve(&notification_souls);

    /* Release the interrupt entry. If no handler we also release "area" which
     * points to a kernel allocated sigevent
     */
    if (!itp->handler) {
        _sfree(itp->area, sizeof(struct sigevent));
    }
    object_free(prp, &interrupt_souls, itp);
}

/* Mask an interrupt level.
 *
 * Increments the level's (and optionally the entry's) mask count, masks the
 * hardware on the 0->1 transition, and broadcasts the mask to other harts when
 * the controller requires it. Parameters:
 *   level : internal interrupt level
 *   itp   : entry whose per-attach mask count to bump (may be NULL)
 * Return value: the level's new mask count.
 */
int rdecl interrupt_mask(unsigned level, interrupt_t *itp)
{
    intr_level_t *ilp;
    int new;

    ilp = &interrupt_level[level];
    unsigned long irq_flags = cpu_irq_save();
    CPU_SLOCK_LOCK(&intr_slock);
    if (itp) {
        ++itp->mask_count;
    }
    if (ilp->mask_count++ == 0) {
        (void) ilp->info->mask(_syspage_ptr, (int) (level - ilp->level_base));
#if CONFIG_PROCESSORS_MAX > 1
        if (pcpu_array[boot_cpunum].pc_alive &&
            ilp->info->flags & INTR_FLAG_SMP_BROADCAST_MASK) {
            extern unsigned hart_ids[];
            unsigned mycpu = get_cpunum();

            for (unsigned _hi = 0; _hi < num_processors; ++_hi) {
                unsigned h = hart_ids[_hi];
                if (h != mycpu)
                    SENDIPI(h, IPI_INTR_MASK);
            }
        }
#endif
    }
    new = ilp->mask_count; /* snarf before intrs enabled, avoid races */
    CPU_SLOCK_UNLOCK(&intr_slock);
    cpu_irq_restore(irq_flags);
    return new;
}

/* Unmask an interrupt level.
 *
 * Decrements the level's (and optionally the entry's) mask count, unmasks the
 * hardware on the 1->0 transition, and broadcasts the unmask to other harts
 * when the controller requires it. Parameters:
 *   level : internal interrupt level
 *   itp   : entry whose per-attach mask count to drop (may be NULL)
 * Return value: the level's new mask count.
 */
int rdecl interrupt_unmask(unsigned level, interrupt_t *itp)
{
    intr_level_t *ilp;
    int new;
    ilp = &interrupt_level[level];
    /* don't enable/disable interrupts during kernel initialization */
    if (pcpu_array[0].pc_alive) {
        CPU_SLOCK_INTR_LOCK(&intr_slock);
    }

    if (itp && itp->mask_count != 0) {
        --itp->mask_count;
    }
    switch (ilp->mask_count) {
    case 0:
        /* already unmasked */
        break;
    case 1:
        (void) ilp->info->unmask(_syspage_ptr, (int) (level - ilp->level_base));
#if CONFIG_PROCESSORS_MAX > 1
        if (pcpu_array[boot_cpunum].pc_alive &&
            ilp->info->flags & INTR_FLAG_SMP_BROADCAST_UNMASK) {
            extern unsigned hart_ids[];
            unsigned mycpu = get_cpunum();

            for (unsigned _hi = 0; _hi < num_processors; ++_hi) {
                unsigned h = hart_ids[_hi];
                if (h != mycpu)
                    SENDIPI(h, IPI_INTR_UNMASK);
            }
        }
#endif
        /* fall through */
    default:
        ilp->mask_count--;
        break;
    }
    new = ilp->mask_count; /* snarf before intrs enabled, avoid races */
    /* don't enable/disable interrupts during kernel initialization */
    if (pcpu_array[0].pc_alive) {
        CPU_SLOCK_INTR_UNLOCK(&intr_slock);
    }
    return new;
}
