/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>

/* command.c */
int roottask_command(ioserv_context_t *ctp, sys_cmd_t *msg);

/* sync.c */
int root_sync_all(int bounded);

/* console.c */
int roottask_console_msg(ioserv_context_t *ctp, sys_console_t *msg);

/* sysconf.c */
int roottask_sysconf_msg(ioserv_context_t *ctp, sys_conf_t *msg);
int roottask_sysconf_set(pid_t pid,
                         unsigned cmd,
                         int name,
                         long value,
                         const char *str);
void roottask_sysconf_destroy(pid_t pid);

/* ids.c */
int proc_getsetid(ioserv_context_t *ctp, proc_getsetid_t *msg);

void proc_dispatch_init(void);

/* loader-context.c */
struct loader_context *loader_context_alloc(unsigned msgsize, int state);
void loader_context_free(struct loader_context *lcp);
void loader_thread_attr(struct _thread_attr *attr,
                        struct loader_context *lcp,
                        ioserv_context_t *ctp);

/* group.c */
int proc_setpgid(ioserv_context_t *ctp, proc_setpgid_t *msg);

/* spawn.c */
int proc_spawn(ioserv_context_t *ctp, void *vmsg);

/* posix-spawn.c */
int proc_pspawn(ioserv_context_t *ctp, void *vmsg);

/* coredump.c */
int proc_coredump(message_context_t *ctp,
                  int code,
                  unsigned flags,
                  void *handle);

/* stack.c */
int proc_stack(message_context_t *ctp, int code, unsigned flags, void *handle);

/* resource.c */
int proc_msg_resource(ioserv_context_t *ctp, void *msg);

/* session.c */
int proc_msg_session(ioserv_context_t *ctp, proc_session_t *msg);

/* event.c */
#define PROC_EVENT_PATHSPACE 0x00000002U
#define PROC_EVENT_SYSCONF 0x00010000U
#define PROC_EVENT_CONFSTR 0x00020000U
#define PROC_EVENT_PRIVILEGED 0xffff0000U

void proc_event_destroy(pid_t pid);
int proc_event(ioserv_context_t *ctp, proc_event_t *msg);
void proc_event_trigger(unsigned flags);

/* umask.c */
int proc_umask(ioserv_context_t *ctp, proc_umask_t *msg);

/* wait.c */
struct wait_entry;
int proc_wait_check(pid_t prp_pid,
                    pid_t parent_pid,
                    struct wait_entry *wap,
                    int event);
int proc_wait(ioserv_context_t *ctp, proc_wait_t *msg);
int proc_stop_or_continue(message_context_t *ctp,
                          int code,
                          unsigned flags,
                          void *handle);
void proc_nozombie(pid_t pid, int status);

/* system-channel.c */
int proc_syschan(ioserv_context_t *ctp, proc_syschan_t *msg);

/* detach.c */
int proc_detach(ioserv_context_t *ctp, proc_detach_t *msg);
int proc_phys_contain(ioserv_context_t *ctp, proc_phys_contain_t *msg);

/* signal.c */
int proc_signal(ioserv_context_t *ctp, proc_kill_t *msg);

/* Job control (proc/jobctl.c). The server argument is the pid of the terminal
 * server asking: ROOTTASK_PID for /dev/text, the driver's pid otherwise.
 */
int proc_msg_jobctl(ioserv_context_t *ctp, proc_jobctl_t *msg);
int proc_jobctl_query(pid_t server, pid_t pid, struct _proc_jobctl_reply *out);
int proc_jobctl_signal_fg(pid_t server, pid_t sid, int signo);
int proc_jobctl_signal_bg(pid_t server, pid_t pid, int signo);
int proc_jobctl_setpgrp(pid_t server, pid_t pid, pid_t pgrp);
int proc_jobctl_tty_busy(pid_t server, pid_t sid);

/* exit.c */
int proc_exit(ioserv_context_t *ctp, union proc_msg_union *msg);
int proc_terminate(message_context_t *ctp,
                   int code,
                   unsigned flags,
                   void *handle);
int ProcessExitMark(pid_t pid, int exit_status);
int ProcessDestroyChancons(pid_t pid, int type_filter);
int ProcessDestroyFdcons(pid_t pid);
int ProcessClearPls(pid_t pid);
int process_clear_lcp(pid_t pid);
pid_t ProcessIndexToPid(unsigned index);
int process_set_ocb_list(pid_t pid, void *head);
int ProcessExitFinalize(pid_t pid);
int ProcessFlagsOp(pid_t pid, uint32_t set, uint32_t clear);
int ProcessSetRlimitCpu(pid_t pid, uint64_t cur, uint64_t max);
int process_set_spawn(pid_t pid,
                      uintptr_t initial_ip,
                      uintptr_t initial_sp,
                      unsigned *flags);
int ProcessListChildren(pid_t parent_pid,
                        pid_t *out_pids,
                        size_t max_pids,
                        size_t *out_count);
int ProcessWaitFinalize(pid_t child_pid,
                        pid_t parent_pid,
                        int match,
                        int *ret_match,
                        int *send_term_notification,
                        unsigned *terming_priority);
int ProcessNozombieMark(pid_t pid,
                        int signo,
                        int code,
                        pid_t pid_for_si,
                        int status_for_si);
