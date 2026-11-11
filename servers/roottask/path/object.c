/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "path-node.h"
#include "path-object.h"
#include "path.h"

#include <sys/path-registry.h>

/* Mark flags for quicker searching later */
static void object_set_flags(node_t *nop, path_object_t *obp)
{
    switch (obp->hdr.type) {
    case OBJECT_SERVER:
        if (!(obp->hdr.flags & NAMESPACE_FLAG_DIR)) {
            nop->flags |= NODE_FLAG_NOCHILD;
        }
        break;
    case OBJECT_PROC_SYMLINK:
        nop->flags |= NODE_FLAG_NONSERVER;
        break;
    default:
        nop->flags |= NODE_FLAG_NONSERVER | NODE_FLAG_NOCHILD;
        break;
    }
}

static void node_entry_bump(node_t *nop)
{
    if (++nop->entry_generation == 0)
        nop->entry_generation++;
}

/* Allocate node entry if necessary, then link object to it. The first objects
 * marked BEFORE of AFTER will win out over later objects marked the same way.
 * If no position reqested the object will be before other objects not marked as
 * BEFORE. The child_count field will be incremented on all ancestors to allow
 * the scaning routine to stop early when checking for objects.
 */
path_object_t *path_object_attach(pid_t creator_pid,
                                  node_t *nop,
                                  const char *path,
                                  int type,
                                  unsigned flags,
                                  void *data,
                                  int *errp)
{
    path_object_t *obp, *p, **pp;
    int admit;

    if (errp)
        *errp = ENOMEM;

    /* Create the node in the pathname space */
    if (!(nop = path_node_lookup(nop, path,
                                 PATH_LOOKUP_ATTACH | PATH_LOOKUP_CREATE, 0))) {
        return 0;
    }

    obp = object_create(type, data, creator_pid);
    if (!obp) {
        path_node_detach(nop);
        return 0;
    }

    CRASHCHECK(obp->hdr.node || obp->hdr.len < sizeof obp->hdr);
    obp->hdr.flags = (uint16_t) flags;

    /* Grab a mutex */
    pthread_mutex_lock(&path_mutex);

    admit = namespace_account_object_admit(creator_pid, obp->hdr.type);
    if (admit != EOK) {
        pthread_mutex_unlock(&path_mutex);
        if (errp)
            *errp = admit;
        path_node_detach(nop);
        object_done(obp);
        return NULL;
    }

    /* Find requested location and link object to node */
    pp = &nop->object;

    if (obp->hdr.type != OBJECT_PROC_SYMLINK) {
        for (; (p = *pp); pp = &p->hdr.next) {
            /* PROC SYMLINKS come before everything */
            if (p->hdr.type == OBJECT_PROC_SYMLINK) {
                continue;
            }

            /* Skip after all object marked BEFORE */
            if (!(p->hdr.flags & NAMESPACE_FLAG_BEFORE)) {
                /* If it wasn't requested to be AFTER link here */
                if (!(obp->hdr.flags & NAMESPACE_FLAG_AFTER)) {
                    break;
                }

                /* Always go before an object marked AFTER */
                if (p->hdr.flags & NAMESPACE_FLAG_AFTER) {
                    break;
                }
            }
        }
    }

    /* Link it to the node */
    obp->hdr.node = nop;
    obp->hdr.next = *pp;
    *pp = obp;
    node_entry_bump(nop);

    /* Set reference count to two - one for the pointer that we're storing in
     * the node, and one for the pointer that the function is returning.
     */
    obp->hdr.refs = 2;

    /* Mark flags for quicker searching later */
    object_set_flags(nop, obp);

    /* Increment child_object count in all ancesters */
    while (nop->parent != nop && (nop = nop->parent)) {
        nop->child_objects++;
    }

    /* Account while the mutex is still held — otherwise a concurrent detach
     * (and its destroy decrement) can overtake this create increment, leaving
     * the destroy as a clamp-at-zero no-op and this create inflating the
     * counter for an object that no longer exists.
     */
    namespace_account_object_create(creator_pid, obp->hdr.type);
    namespace_version_bump();

    /* Free the mutex */
    pthread_mutex_unlock(&path_mutex);

    /* Diagnostic: log every successful attach. Visible at -v (roottask_loglevel
     * >= 1). Type-name table mirrors the enum in
     * servers/roottask/include/path-object.h:21-30. pid is the process that
     * owns the I/O service (roottask itself for the built-in /dev/null /dev/tty
     * /sys/X set; an external pid for serial drivers and other I/O services).
     */
    {
        static const char *const type_names[] = {
            [OBJECT_NONE] = "none",         [OBJECT_SERVER] = "server",
            [OBJECT_NAME] = "name",         [OBJECT_PROC_SYMLINK] = "symlink",
            [OBJECT_MEM_ANON] = "mem-anon", [OBJECT_MEM_SHARED] = "mem-shared",
            [OBJECT_MEM_FD] = "mem-fd",     [OBJECT_MEM_TYPED] = "mem-typed",
        };
        const char *tname =
            (obp->hdr.type < (int) (sizeof(type_names) / sizeof(*type_names)) &&
             type_names[obp->hdr.type])
                ? type_names[obp->hdr.type]
                : "?";
        root_dbg("namespace: attached %s '%s' (pid=%d, flags=0x%x)\n", tname,
                 path ? path : "<null>", (int) creator_pid, flags);
    }

    return obp;
}


/* This will remove an object from its neighbouring objects, then unlink from
 * the node. The object must be freed by the calling routine after the unlink.
 */
void path_object_detach(path_object_t *obp)
{
    path_object_t *p, **pp;
    node_t *nop, *anc;
    pid_t creator;
    int type;
#ifndef NDEBUG
    bool found = false;
#endif

    /* Grab a mutex */
    pthread_mutex_lock(&path_mutex);

    nop = obp->hdr.node;
    creator = obp->hdr.creator;
    type = obp->hdr.type;
    obp->hdr.node = NULL;

    /* Unset these flags, they will be reset in the loop below */
    nop->flags &= (uint16_t) ~(NODE_FLAG_NONSERVER | NODE_FLAG_NOCHILD);

    /* Scan the objects to remove this one */
    for (pp = &nop->object; (p = *pp); pp = &p->hdr.next) {
        if (p == obp) {
#ifndef NDEBUG
            found = true;
#endif
            node_entry_bump(nop);
            if (!(p = *pp = p->hdr.next)) {
                break;
            }
        }
        /* Mark flags for quicker searching later */
        object_set_flags(nop, p);
    }
#ifndef NDEBUG
    if (!found)
        crash();
#endif

    /* Decrement child_object count on all ancestors */
    anc = nop;
    while (anc->parent != anc && (anc = anc->parent)) {
        CRASHCHECK(anc->child_objects == 0);
        anc->child_objects--;
    }

    /* Account before releasing the mutex — pairs with the inside-mutex
     * accounting in path_object_attach so concurrent create/destroy on the same
     * node can't reorder into a phantom counter.
     */
    namespace_account_object_destroy(creator, type);
    namespace_version_bump();

    /* Free the mutex */
    pthread_mutex_unlock(&path_mutex);

    /* Now release the node */
    path_node_detach(nop);

    /* Detach the object */
    path_object_done(obp);
}


size_t path_object_pathname(path_object_t *obp, size_t max, char *dest)
{
    int raw_len;

    pthread_mutex_lock(&path_mutex);
    if (obp->hdr.node) {
        raw_len = path_node_fullpath(obp->hdr.node, dest, (int) max);
        if (raw_len < 0) {
            raw_len = 0;
        }
    } else {
        raw_len = 0;
    }
    pthread_mutex_unlock(&path_mutex);
    return (size_t) raw_len;
}


path_object_t *path_object_clone(path_object_t *obp)
{
    atomic_add(&obp->hdr.refs, 1);
    return obp;
}


void path_object_unclone(path_object_t *obp)
{
    atomic_sub(&obp->hdr.refs, 1);
}


void path_object_done(path_object_t *obp)
{
    if (atomic_sub_value(&obp->hdr.refs, 1) == 1) {
        object_done(obp);
    }
}


static int dummy_done(path_object_t *o)
{
    return 1;
}

static size_t dummy_name(path_object_t *o, size_t max, char *dst)
{
    if (dst)
        dst[0] = '\0';
    return 0;
}

static int dummy_devctl(ioserv_context_t *ctp,
                        io_devctl_t *msg,
                        path_object_t *obp)
{
    return ENOSYS;
}

extern int server_create(path_object_t *, void *);
extern int symlink_create(path_object_t *, void *);
extern int anmem_create(path_object_t *, void *);
extern int anmem_done(path_object_t *);
extern size_t anmem_name(path_object_t *, size_t, char *);
extern int shmem_create(path_object_t *, void *);
extern int shmem_done(path_object_t *);
extern size_t shmem_name(path_object_t *, size_t, char *);
extern int shmem_devctl(ioserv_context_t *ctp,
                        io_devctl_t *msg,
                        path_object_t *obp);
extern int fdmem_create(path_object_t *, void *);
extern int fdmem_done(path_object_t *);
extern size_t fdmem_name(path_object_t *, size_t, char *);
extern int tymem_create(path_object_t *, void *);
extern int tymem_done(path_object_t *);

struct object_info {
    unsigned len;
    int (*create)(path_object_t *, void *);
    int (*done)(path_object_t *);
    size_t (*name)(path_object_t *, size_t, char *);
    int (*devctl)(ioserv_context_t *ctp, io_devctl_t *msg, path_object_t *obp);
};

static const struct object_info obj_info[] = {
    [OBJECT_SERVER] =
        {
            .len = sizeof(struct server_object),
            .create = server_create,
            .done = dummy_done,
            .name = dummy_name,
            .devctl = dummy_devctl,
        },
    [OBJECT_NAME] =
        {
            .len = sizeof(struct server_object),
            .create = server_create,
            .done = dummy_done,
            .name = dummy_name,
            .devctl = dummy_devctl,
        },
    [OBJECT_PROC_SYMLINK] =
        {
            .create = symlink_create,
            .done = dummy_done,
            .name = dummy_name,
            .devctl = dummy_devctl,
        },
    [OBJECT_MEM_ANON] =
        {
            .len = sizeof(struct mm_object_anon),
            .create = anmem_create,
            .done = anmem_done,
            .name = anmem_name,
            .devctl = dummy_devctl,
        },
    [OBJECT_MEM_SHARED] =
        {
            .len = sizeof(struct mm_object_shared),
            .create = shmem_create,
            .done = shmem_done,
            .name = shmem_name,
            .devctl = shmem_devctl,
        },
    [OBJECT_MEM_FD] =
        {
            .len = sizeof(struct mm_object_fd),
            .create = fdmem_create,
            .done = fdmem_done,
            .name = fdmem_name,
            .devctl = dummy_devctl,
        },
    [OBJECT_MEM_TYPED] =
        {
            .len = sizeof(struct mm_object_typed),
            .create = tymem_create,
            .done = tymem_done,
            .name = dummy_name,
            .devctl = dummy_devctl,
        },
};


path_object_t *object_create(int type, void *extra, pid_t creator_pid)
{
    const struct object_info *info;
    unsigned len;
    path_object_t *obp;

    info = &obj_info[type];
    len = info->len;
    if (len == 0) {
        int create_len = info->create(NULL, extra);
        if (create_len < 0)
            return NULL;
        len = (unsigned) create_len;
        if (len > PATH_MAX_OBJECT_LEN) {
            /* too big to fit into hdr.len. We can simply return because nothing
             * was alloced since we passed NULL for obp
             */
            return NULL;
        }
    }
    obp = _scalloc(len);
    if (obp) {
        obp->hdr.type = (uint8_t) type;
        CRASHCHECK(len > PATH_MAX_OBJECT_LEN);
        obp->hdr.len = (uint8_t) len;
        obp->hdr.creator = creator_pid;

        if (info->create(obp, extra) != EOK) {
            _sfree(obp, obp->hdr.len);
            obp = NULL;
        }
    }
    return obp;
}

#define OBJECT_DONE_INPROGRESS 0x80000000

int object_done(path_object_t *obp)
{
    int r = 0;

    if (obp->hdr.refs == 0) {
        atomic_or(&obp->hdr.refs, OBJECT_DONE_INPROGRESS);
        r = obj_info[obp->hdr.type].done(obp);
        atomic_clr(&obp->hdr.refs, OBJECT_DONE_INPROGRESS);
        if (r) {
            free(obp);
        }
    }
    return r;
}


size_t object_name(path_object_t *obp, size_t max, char *name)
{
    return obj_info[obp->hdr.type].name(obp, max, name);
}


int object_devctl(ioserv_context_t *ctp, io_devctl_t *msg, path_object_t *obp)
{
    return obj_info[obp->hdr.type].devctl(ctp, msg, obp);
}
