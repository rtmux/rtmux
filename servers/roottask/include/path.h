/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PATH_H
#define PATH_H

#include <path-object.h>

struct object_funcs;

/* path-init.c */
void path_init(void);
int roottask_namespace_link(const char *path,
                            uint32_t nd,
                            pid_t pid,
                            int chid,
                            unsigned handle,
                            enum _file_type file_type,
                            unsigned flags);

/* path-link.c */
extern const struct object_funcs server_funcs;
struct symlink_object;
int reply_symlink(ioserv_context_t *ctp,
                  unsigned eflag,
                  struct _io_connect_link_reply *link,
                  struct symlink_object *symlink,
                  const char *path,
                  const char *tail);
int path_link(ioserv_context_t *ctp,
              io_link_t *msg,
              void *handle,
              io_link_extra_t *extra);
int path_node_fullpath(node_t *node, char *path, int pathmax);

/* path-open.c */
int path_open(ioserv_context_t *ctp, io_open_t *msg, void *handle, void *extra);
int path_unlink(ioserv_context_t *ctp,
                io_unlink_t *msg,
                void *handle,
                void *extra);
int path_readlink(ioserv_context_t *ctp,
                  io_readlink_t *msg,
                  void *handle,
                  void *extra);

struct node_entry *path_resolve_path(struct node_entry *node,
                                     struct _io_connect *connect,
                                     char *path,
                                     struct node_entry *root,
                                     unsigned extra_lookup_flags);
int path_resolve_servers(ioserv_context_t *ctp,
                         struct node_entry *node,
                         struct _io_connect *connect,
                         const char *path,
                         struct node_entry *root);

/* path-stats.c */
enum namespace_roundtrip_op {
    NAMESPACE_RT_OPEN = 0,
    NAMESPACE_RT_READLINK,
    NAMESPACE_RT_UNLINK,
    NAMESPACE_RT_CHDIR,
    NAMESPACE_RT_CHROOT,
    NAMESPACE_RT_RESOLVE,
    NAMESPACE_RT_ROOT_RESOLVE,
    NAMESPACE_RT_NOPS
};

void namespace_stats_roundtrip(enum namespace_roundtrip_op op);
void namespace_stats_lookup(int uncacheable);
void namespace_version_bump(void);
unsigned long long namespace_version_read(void);
size_t namespace_stats_snapshot(char *buf, size_t cap);

/* sync.c */
#define SYNC_MAX_TARGETS 16
#define SYNC_LABEL_MAX 96

struct sync_target {
    uint32_t nd;
    pid_t pid;
    int chid;
    char label[SYNC_LABEL_MAX];
};

size_t namespace_sync_snapshot(struct sync_target *out, size_t max);

/* accounting.c */
int namespace_account_object_admit(pid_t creator_pid, int type);
void namespace_account_object_create(pid_t creator_pid, int type);
void namespace_account_object_destroy(pid_t creator_pid, int type);
size_t namespace_accounting_snapshot(char *buf, size_t cap);

node_t *path_node_alloc(const char *name, unsigned len);
node_t *path_node_lookup(node_t *nop,
                         const char *path,
                         unsigned flags,
                         const char **result);
void path_node_detach(node_t *nop);
void path_node_detach_flags(node_t *nop, unsigned);
void path_node_access(node_t *node);
void path_node_complete(node_t *node);
node_t *path_node_clone(node_t *node);

/* Nonzero if any object attached to nop carries NAMESPACE_FLAG_UNCACHEABLE.
 * Caller must hold path_mutex (taken implicitly by path_node_lookup or
 * explicitly via path_node_access/complete).
 */
int path_node_is_uncacheable(const node_t *nop);

path_object_t *path_object_attach(pid_t creator_pid,
                                  node_t *nop,
                                  const char *path,
                                  int type,
                                  unsigned flags,
                                  void *data,
                                  int *errp);
void path_object_detach(path_object_t *obp);
size_t path_object_pathname(path_object_t *obp, size_t max, char *dest);
path_object_t *path_object_clone(path_object_t *);
void path_object_unclone(path_object_t *);
void path_object_done(path_object_t *);

path_object_t *object_create(int type, void *extra, pid_t creator_pid);
int object_done(path_object_t *);
size_t object_name(path_object_t *, size_t, char *);
int object_devctl(ioserv_context_t *, io_devctl_t *, path_object_t *);

#endif
