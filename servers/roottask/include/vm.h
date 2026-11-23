/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef VM_H
#define VM_H

#include <path-object.h>

/* vm-init.c */
void vm_init(void);
int vm_resize(path_object_t *obp, size_t size);
extern const struct object_funcs mem_funcs;

/* mem/as/resize.c, each called with no object or address-space lock held.
 * vm_grow grows an object and makes its existing mappings reach the new pages;
 * vm_undo_grow takes back the growth of a write that failed; vm_grow_drain makes
 * the mappings of the growths that paths holding locks queued.
 */
struct vm_growth {
    uint64_t from; /* size before vm_grow */
    uint64_t to;   /* size after it */
    uint64_t gen;  /* the object's write generation it set */
};
int vm_grow(path_object_t *obp, size_t size, struct vm_growth *g);
void vm_undo_grow(path_object_t *obp, const struct vm_growth *g);
void vm_grow_drain(path_object_t *grown);

/* vm-map.c */
int vm_find_object(ioserv_context_t *ctp,
                   pid_t prp_pid,
                   process_t *prp,
                   int fd,
                   mem_map_t *msg,
                   path_object_t **obpp);
int vm_map(ioserv_context_t *ctp,
           pid_t prp_pid,
           process_t *prp,
           mem_map_t *msg,
           path_object_t **grownp);
int vm_handle(ioserv_context_t *ctp,
              pid_t prp_pid,
              process_t *prp,
              mem_handle_t *msg);
int memcap_lookup_map(ioserv_context_t *ctp,
                      uint64_t handle,
                      unsigned prot,
                      path_object_t **obpp,
                      uint32_t *flagsp);

/* vm-ctrl.c */
int vm_ctrl(pid_t prp_pid, process_t *prp, mem_ctrl_t *msg);

/* vm-info.c */
int vm_info(ioserv_context_t *ctp,
            pid_t prp_pid,
            process_t *prp,
            mem_info_t *msg);

/* vm-offset.c */
int vm_offset(ioserv_context_t *ctp,
              pid_t prp_pid,
              process_t *prp,
              mem_offset_t *msg);

/* vm-debug-info.c */
int vm_debug_info(ioserv_context_t *ctp,
                  pid_t prp_pid,
                  process_t *prp,
                  mem_debug_info_t *msg);

/* vm-fd.c */
void vm_fd_init(void);
int vm_open_fd(ioserv_context_t *ctp,
               pid_t prp_pid,
               process_t *prp,
               mem_map_t *msg,
               path_object_t **obp);
int vm_fd_compact(void);
unsigned vm_fd_zombie_count(void);
void vm_fd_describe(char *buf, size_t len);
int vm_fd_setname(path_object_t *obp, char *name);

/* vm-support.c — adp lock state migrated to root_process_t; signatures take
 * pid_t directly so callers no longer need to deref a kernel-half process_t to
 * reach it. Was process_t *.
 */
int rdecl proc_rlock_adp(pid_t pid);
int rdecl proc_wlock_adp(pid_t pid);
int rdecl proc_unlock_adp(pid_t pid);
int rdecl proc_rlock_promote_adp(pid_t pid);
void rdecl proc_lock_owner_mark(pid_t pid);

int vm_tymem_open(const char *path,
                  int oflags,
                  path_object_t **obpp,
                  pid_t creator_pid);
void vm_tymem_close(path_object_t *obp);

#endif
