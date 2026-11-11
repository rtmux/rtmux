/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

struct mem_ocb;
struct hwres_range;
struct proc_mux_lock;

/* message.c */
void message_init(void);
int message_start(void);
int proc_thread_pool_reserve(void);
int proc_thread_pool_reserve_done(void);

/* support.c */
process_t *root_proc_lookup(pid_t pid);
process_t *root_proc_lock(pid_t pid);

/* Lock the parent of "child_pid" (looked up via root_proc()->parent_pid). The
 * parent's pid is written through "parent_pid_out" if non-NULL — callers need
 * it for the matching root_proc_unlock(parent_pid) and avoid the now-illegal
 * deref of parent->pid.
 */
process_t *root_proc_lock_parent(pid_t child_pid, pid_t *parent_pid_out);
void root_proc_unlock(pid_t pid);
int proc_error(int ret, pid_t pid);
int proc_isaccess(process_t *prp, struct _msg_info *rcvinfo);
void *proc_object_alloc(soul_t *soulp);
void proc_object_free(soul_t *soulp, void *ptr);
int proc_status(ioserv_context_t *ctp, int status);
int rdecl proc_mux_lock(struct proc_mux_lock **mp);
int rdecl proc_mux_unlock(struct proc_mux_lock **mp);
int rdecl proc_mux_haslock(struct proc_mux_lock **mp, unsigned owner);

/* proc-loader.c */
int loader_exit(ioserv_context_t *hdr, union proc_msg_union *msg, pid_t pid);
void *loader_load(void *lcp);

/* devnull.c */
void devnull_init(void);

/* devtty.c */
void devtty_init(void);

/* devstd.c */
void devstd_init(void);

/* devzero.c */
void devzero_init(void);
int devzero_check(const ioserv_ops_t *funcs,
                  mem_map_t *msg,
                  void *h,
                  path_object_t **pobp);

/* devmem.c */
int devmem_check(const ioserv_ops_t *funcs,
                 mem_map_t *msg,
                 void *h,
                 path_object_t **pobp);
int mem_open(ioserv_context_t *ctp, io_open_t *msg, void *handle, void *extra);
int tymem_open(ioserv_context_t *ctp,
               io_open_t *msg,
               void *handle,
               void *extra);
void devmem_init(void);

/* named semaphores */
void namedsem_init(void);

/* procfs.h */
void procfs_wait_stale(pid_t pid);
void procfs_init(void);

/* devtext.c */
void devtext_init(void);

/* bootimage.c */
void bootimage_init(void);

/* init.c */
void roottask_conf_init(void);
const struct build_info_entry *syspage_build_info(void);

/* hwres-init.c */
void hwres_init(void);
void hwres_destroy_process(pid_t pid);
int hwres_proc_interface(void *list, unsigned count, int command);
int hwres_proc_query(struct hwres_range *list, int count, int start, int flags);
int hwres_proc_query_pid(pid_t pid,
                         struct hwres_range *list,
                         int count,
                         int start,
                         int flags);
int hwres_proc_devno(char *name, dev_t *devno, int minor_request, int flags);

/* proc-read.c */
ssize_t proc_read(int fd, void *buff, ssize_t nbytes, off64_t off);

/* loader-elf.c */
struct loader_startup;
struct stat;
int elf_load(int fd,
             const char *path,
             struct loader_startup *lsp,
             struct stat *stat,
             struct inheritance *parms);
