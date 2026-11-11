/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if !defined(KARGSLOT)
#define KARGSLOT(arg) arg
#endif

typedef union kcall_args {
    struct kcall_args_null {
        KARGSLOT(int32_t dummy);
    } null;

    struct kcall_args_dbgprint {
        KARGSLOT(const char *str);
        KARGSLOT(size_t len);
    } dbgprint;

    struct kcall_args_channel_create {
        KARGSLOT(uint32_t flags);

        /* Slot 1 (a1). Out pointer for the minted gating cap id, written only
         * under RTMUX_FLG_CHF_CAP_GATED; the handler ignores it otherwise, so
         * plain channel_create() callers are unaffected. This was the unused
         * "mode" slot; keeping it here preserves the a2..a5 positions of the
         * async bufsize/maxbuf/event/cred slots.
         */
        KARGSLOT(uint64_t *cap_out);
        KARGSLOT(size_t bufsize);
        KARGSLOT(unsigned maxbuf);
        KARGSLOT(struct sigevent *event);
        KARGSLOT(struct _cred_info *cred);
    } channel_create;

    struct kcall_args_channel_destroy {
        KARGSLOT(int32_t chid);
    } channel_destroy;

    struct kcall_args_connect_attach {
        KARGSLOT(uint32_t nd);
        KARGSLOT(pid_t pid);
        KARGSLOT(uint32_t chid);
        KARGSLOT(uint32_t index);
        KARGSLOT(int32_t flags);
    } connect_attach;

    struct kcall_args_connect_detach {
        KARGSLOT(uint64_t coid);
    } connect_detach;

    struct kcall_args_connect_client_info {
        KARGSLOT(int32_t scoid);
        KARGSLOT(struct _client_info *info);
        KARGSLOT(int32_t ngroups);
    } connect_client_info;

    struct kcall_args_connect_server_info {
        KARGSLOT(pid_t pid);
        KARGSLOT(uint64_t coid);
        KARGSLOT(struct _server_info *info);
    } connect_server_info;

    struct kcall_args_connect_flags {
        KARGSLOT(int32_t pid);
        KARGSLOT(uint64_t coid);
        KARGSLOT(int32_t mask);
        KARGSLOT(int32_t bits);
    } connect_flags;

    struct kcall_args_msg_sendv {
        KARGSLOT(uint64_t coid);
        KARGSLOT(iov_t *smsg);
        KARGSLOT(int32_t sparts);
        KARGSLOT(iov_t *rmsg);
        KARGSLOT(int32_t rparts);
    } msg_sendv;

    struct kcall_args_msg_receivev {
        KARGSLOT(int32_t chid);
        KARGSLOT(iov_t *rmsg);
        KARGSLOT(int32_t rparts);
        KARGSLOT(struct _msg_info *info);
        KARGSLOT(uint32_t coid);
    } msg_receivev;

    struct kcall_args_msg_replyv {
        KARGSLOT(int32_t rcvid);
        KARGSLOT(int32_t status);
        KARGSLOT(iov_t *smsg);
        KARGSLOT(int32_t sparts);
    } msg_replyv;

    struct kcall_args_msg_readv {
        KARGSLOT(int32_t rcvid);
        KARGSLOT(iov_t *rmsg);
        KARGSLOT(int32_t rparts);
        KARGSLOT(uint32_t offset);
    } msg_readv;

    struct kcall_args_msg_writev {
        KARGSLOT(int32_t rcvid);
        KARGSLOT(iov_t *smsg);
        KARGSLOT(int32_t sparts);
        KARGSLOT(uint32_t offset);
    } msg_writev;

    /* kcall_args_msg_readwritev removed — slot bound to ksys_bad. */

    struct kcall_args_msg_readiov {
        KARGSLOT(int32_t rcvid);
        KARGSLOT(iov_t *iov);
        KARGSLOT(int32_t parts);
        KARGSLOT(uint32_t offset);
        KARGSLOT(uint32_t flags);
    } msg_readiov;

    struct kcall_args_msg_info {
        KARGSLOT(int32_t rcvid);
        KARGSLOT(struct _msg_info *info);
    } msg_info;

    struct kcall_args_msg_sendnotification {
        KARGSLOT(uint64_t coid);
        KARGSLOT(int32_t priority);
        KARGSLOT(int32_t code);
        KARGSLOT(int32_t value);
    } msg_sendnotification;

    struct kcall_args_msg_deliver_event {
        KARGSLOT(int32_t rcvid);
        KARGSLOT(struct sigevent *event);
    } msg_deliver_event;

    struct kcall_args_msg_verify_event {
        KARGSLOT(int32_t rcvid);
        KARGSLOT(struct sigevent *event);
    } msg_verify_event;

    struct kcall_args_msg_keydata {
        KARGSLOT(int32_t rcvid);
        KARGSLOT(int32_t op);
        KARGSLOT(uint32_t key);
        KARGSLOT(uint32_t *newkey);
        KARGSLOT(iov_t *msg);
        KARGSLOT(int32_t parts);
    } msg_keydata;

    struct kcall_args_msg_error {
        KARGSLOT(int32_t rcvid);
        KARGSLOT(int32_t err);
    } msg_error;

    struct kcall_args_msg_current {
        KARGSLOT(int32_t rcvid);
    } msg_current;

    struct kcall_args_thread_create {
        KARGSLOT(pid_t pid);
        KARGSLOT(void (*func)(union sigval));
        KARGSLOT(void *arg);
        KARGSLOT(struct _thread_attr *attr);
    } thread_create;

    struct kcall_args_thread_destroy {
        KARGSLOT(int32_t tid);
        KARGSLOT(int32_t priority);
        KARGSLOT(void *status);
    } thread_destroy;

    struct kcall_args_thread_detach {
        KARGSLOT(int32_t tid);
    } thread_detach;

    struct kcall_args_thread_cancel {
        KARGSLOT(int32_t tid);
        KARGSLOT(void (*canstub)());
    } thread_cancel;

    struct kcall_args_thread_join {
        KARGSLOT(int32_t tid);
        KARGSLOT(void **status);
    } thread_join;

    struct kcall_args_thread_ctl {
        KARGSLOT(int32_t cmd);
        KARGSLOT(void *data);
    } thread_priv;

    struct kcall_args_sched_get {
        KARGSLOT(pid_t pid);
        KARGSLOT(int32_t tid);
        KARGSLOT(struct sched_param *param);
    } sched_get;

    struct kcall_args_sched_set {
        KARGSLOT(pid_t pid);
        KARGSLOT(int32_t tid);
        KARGSLOT(int32_t policy);
        KARGSLOT(struct sched_param *param);
    } sched_set;

    struct kcall_args_sched_info {
        KARGSLOT(pid_t pid);
        KARGSLOT(int policy);
        KARGSLOT(struct _sched_info *info);
    } sched_info;

    struct kcall_args_interrupt_attach_thread {
        KARGSLOT(uint32_t intr);
        KARGSLOT(uint32_t flags);
    } interrupt_attach_thread;

    struct kcall_args_interrupt_attach_event {
        KARGSLOT(uint32_t intr);
        KARGSLOT(const struct sigevent *event);
        KARGSLOT(uint32_t flags);
    } interrupt_attach_event;

    struct kcall_args_interrupt_detach {
        KARGSLOT(uint32_t id);
    } interrupt_detach_id;

    struct kcall_args_interrupt_wait {
        KARGSLOT(uint32_t flags);
        KARGSLOT(uint64_t *timeout);
    } interrupt_wait;

    struct kcall_args_interrupt_mask {
        KARGSLOT(uint32_t intr);
        KARGSLOT(uint32_t id);
    } interrupt_mask;

    struct kcall_args_interrupt_unmask {
        KARGSLOT(uint32_t intr);
        KARGSLOT(uint32_t id);
    } interrupt_unmask;

    struct kcall_args_sync_create {
        KARGSLOT(unsigned type);
        KARGSLOT(sync_t *sync);
        KARGSLOT(sync_attr_t *attr);
    } sync_create;

    struct kcall_args_sync_destroy {
        KARGSLOT(sync_t *sync);
    } sync_destroy;

    struct kcall_args_sync_mutex_lock {
        KARGSLOT(sync_t *sync);
    } sync_mutex_lock;

    struct kcall_args_sync_mutex_unlock {
        KARGSLOT(sync_t *sync);
    } sync_mutex_unlock;

    struct kcall_args_sync_mutex_revive {
        KARGSLOT(sync_t *sync);
    } sync_mutex_revive;

    struct kcall_args_sync_ctl {
        KARGSLOT(int cmd);
        KARGSLOT(sync_t *sync);
        KARGSLOT(void *data);
    } sync_ctl;

    struct kcall_args_sync_condvar_wait {
        KARGSLOT(sync_t *sync);
        KARGSLOT(sync_t *mutex);
    } sync_condvar_wait;

    struct kcall_args_sync_condvar_signal {
        KARGSLOT(sync_t *sync);
        KARGSLOT(int32_t all);
    } sync_condvar_signal;

    struct kcall_args_sync_sem_wait {
        KARGSLOT(sync_t *sync);
        KARGSLOT(int32_t try);
    } sync_sem_wait;

    struct kcall_args_sync_sem_post {
        KARGSLOT(sync_t *sync);
    } sync_sem_post;

    struct kcall_args_clock_time {
        KARGSLOT(clockid_t id);
        KARGSLOT(uint64_t *new);
        KARGSLOT(uint64_t *old);
    } clock_time;

    struct kcall_args_clock_period {
        KARGSLOT(clockid_t id);
        KARGSLOT(struct _clockperiod *new);
        KARGSLOT(struct _clockperiod *old);
        KARGSLOT(int32_t external);
    } clock_period;

    struct kcall_args_clock_id {
        KARGSLOT(int32_t pid);
        KARGSLOT(int32_t tid);
    } clock_id;

    struct kcall_args_timer_create {
        KARGSLOT(clockid_t id);
        KARGSLOT(struct sigevent *event);
    } timer_create;

    struct kcall_args_timer_destroy {
        KARGSLOT(int32_t id);
    } timer_destroy;

    struct kcall_args_timer_settime {
        KARGSLOT(timer_t id);
        KARGSLOT(int32_t flags);
        KARGSLOT(struct _itimer *itime);
        KARGSLOT(struct _itimer *oitime);
    } timer_settime;

    struct kcall_args_timer_info {
        KARGSLOT(pid_t pid);
        KARGSLOT(timer_t id);
        KARGSLOT(int32_t flags);
        KARGSLOT(struct _timer_info *info);
    } timer_info;

    struct kcall_args_timer_timeout {
        KARGSLOT(clockid_t id);
        KARGSLOT(uint32_t timeout_flags);
        KARGSLOT(struct sigevent *event);
        KARGSLOT(uint64_t *ntime);
        KARGSLOT(uint64_t *otime);
    } timer_timeout;

    struct kcall_args_priv {
        KARGSLOT(int id);    /* sub-namespace ID (__KSYS_PRIV_*) */
        KARGSLOT(void *arg); /* opaque argument to handler   */
    } priv;

    struct kcall_args_trace_event {
        KARGSLOT(uint32_t *data);
    } trace_event;

    struct kcall_args_sys_cpupage_get {
        KARGSLOT(int32_t index);
    } sys_cpupage_get;

    struct kcall_args_sys_cpupage_set {
        KARGSLOT(int32_t index);
        KARGSLOT(intptr_t value);
    } sys_cpupage_set;
} kcall_args_t;
