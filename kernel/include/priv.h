/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Covers for ksys_priv calls */

/* Kernel calls which are installed by the proc service */
int as_memcpy(pid_t target_pid,
              void *target_va,
              void *roottask_buf,
              size_t len,
              int direction);
#define AS_MEMCPY_FROM_TARGET 0
#define AS_MEMCPY_TO_TARGET 1

int cred_get_call(pid_t pid, struct _cred_info *cip);
int cred_set_call(pid_t pid, struct _cred_info *cip);
void debug_install(int code);
int debug_attach(pid_t pid, unsigned flags);
int debug_detach(pid_t pid);
int debug_process_call(enum rtmux_debug_request request,
                       pid_t pid,
                       int tid,
                       union rtmux_debug_data *data);
process_t *process_create(pid_t parent_pid, void *lcp, pid_t *out_pid);
int process_destroy_call(pid_t pid);
int process_destroy_all(pid_t pid);
int process_exec(void *lcp, int *prcvid, int rcvid, int ppid);
int process_shutdown(process_t *prp, int exec);
void process_reparent(process_t *prp);
void signal_kill(channel_t *chp, int signo, pid_t sender_pid);
void process_threads_destroy(process_t *prp);
int process_timer_destroy(process_t *prp, int id);
int process_connect_detach(process_t *prp, int coid);
int process_bind(pid_t pid);
void *process_startup(pid_t pid, int start, uint64_t priv_caps);
void *query_object(unsigned type,
                   unsigned index1,
                   int subtype,
                   unsigned index2,
                   unsigned *next,
                   void *objbuf,
                   size_t objsize);
void query_object_done(pid_t pid);
int channel_has_receivers(pid_t pid, int chid);
int roottask_mmap_phys_user(paddr_t paddr,
                            size_t size,
                            int prot,
                            uintptr_t vaddr_hint,
                            uintptr_t *vaddr_out);
int roottask_mmap_anon_user(size_t size, int prot, uintptr_t *vaddr_out);
int kernel_page_zero(paddr_t paddr, size_t len);
int kernel_icache_sync_all(void);
struct _thread_local_storage *thread_tls(struct _thread_local_storage *tls,
                                         uintptr_t stackaddr,
                                         size_t stacksize,
                                         size_t guardsize,
                                         uintptr_t esp);
void kext_lock(void);
void kext_status(thread_t *thp, int status);
void system_reboot(int);
void system_shutdown(void);
int stack_cont(pid_t pid, int tid, int err, struct _thread_attr *attr);
int stack_wait_info(union sigval value,
                    pid_t *ppid,
                    int *ptid,
                    struct _thread_attr *attr);
int kext_thread_ctl_call(process_t *prp, int tid, int32_t cmd, void *data);
void memobj_destroyed(path_object_t *obp,
                      paddr_t start,
                      paddr_t end,
                      process_t *prp,
                      void *vaddr);
int kext_add_trace_event(int ev_class, int ev_code, void *data, int datalen);
int kext_add_trace_event_iov(int ev_class,
                             int ev_code,
                             iov_t *iovs,
                             int iovlen);
