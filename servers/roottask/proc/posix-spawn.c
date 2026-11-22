/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include <limits.h>
#include <spawn.h>
#include <sys/spawn-action-wire.h>
#include <sys/spawn-limits.h>

#define SPAWN_FD_TARGET_MAX (UINT16_MAX - 1)


/* 							Internal Support Routines */

/** key_mask_create
 *
 * All of the libc posix_spawnattr_xx() and posix_spawn_file_actions_xx()
 * routines use KEY_MASK to ensure that the keyed values are 8 byte aligned.
 * When the data is messaged to proc, there is no way to guarantee that this
 * alignment will be preserved during the copy. In order to make use of the
 * posix_spawnattr_xx() and posix_spawn_file_actions_xx() routines to parse the
 * incoming message, we locally reconstruct the 'posix_spawnattr_t' and
 * 'posix_spawn_file_actions_t' objects however we must cause the libc routines
 * to use an alternate key mask (ie. one that is acceptable to the natural
 * alignment of the '_posix_spawnattr_t' and '_posix_spawn_file_actions_t' with
 * the message sent to us.) This is done by using the KEY_MASK bits to specify
 * the key mask in the pointer passed as the '_posix_spawnattr_t' or
 * '_posix_spawn_file_actions_t' object (see GET_ATTRP() and GET_FACTP() in
 * libc). The net result is that if the messaged structures are aligned on an 8
 * byte boundary, we simply use KEY_MASK as expected by the libc routines. If
 * its 4 byte aligned (the only other option), then we use a key mask of 3 and
 * encode the mask into the KEY_MASK bits of the '_posix_spawnattr_t' or
 * '_posix_spawn_file_actions_t' objects
 *
 * The alternative to this solution is to
 * 		- weaken KEY_MASK by setting is to 3 requiring only 4 byte alignment
 * 		  which increases the chance of bogus pointer detection for user
 * programs failing in the libc routines
 * 		- memmove() the messaged structures to an 8 byte alignment boundary
 * which is costly and in efficient
 */
#define ALIGNOF(x) ((uintptr_t) (x) & KEY_MASK)

static uintptr_t key_mask_create(void *_v)
{
    uintptr_t v = (uintptr_t) _v;
    uintptr_t align = ALIGNOF(v);
    CRASHCHECK((align != 0) && (align != 4));
    return (align == 0) ? KEY_MASK : align - 1;
}

/* obj_key_adj — adjust a reconstructed-object pointer for the alignment of the
 * underlying messaged buffer, complementing key_mask_create. Parameters:
 *   _a : pointer to the locally reconstructed attr/factions object
 *   _b : pointer to the messaged buffer whose alignment is used
 * Return value: the alignment-adjusted object pointer.
 */
static uintptr_t obj_key_adj(void *_a, void *_b)
{
    uintptr_t a = (uintptr_t) _a;
    uintptr_t b = (uintptr_t) _b;
    uintptr_t align = ALIGNOF(b);
    CRASHCHECK((align != 0) && (align != 4));
    return a + ((align == 0) ? 0 : align - 1);
}

/* True when a region of the message starts where key_mask_create can handle it.
 * The sender chooses attr_bytes, so the file actions can start anywhere.
 */
static bool pspawn_region_aligned(const void *p)
{
    uintptr_t align = ALIGNOF(p);

    return align == 0 || align == 4;
}

/** fill_struct_inheritance
 *
 * Populate a struct inheritance from a posix_spawnattr_t by extracting each
 * attribute (flags, pgroup, signal masks, stack max, sched policy, node,
 * runmask, sched param) via the libc posix_spawnattr_getxxx() accessors.
 *
 * Returns: EOK on success, otherwise an errno from the failing accessor.
 */
static int fill_struct_inheritance(struct inheritance *inherit,
                                   posix_spawnattr_t *attrp)
{
    int r;

    CRASHCHECK(!inherit);

    short flags;

    if ((r = posix_spawnattr_getflags(attrp, &flags)) != EOK) {
        return r;
    }

    /* The flags come from the client's message: zero extend them, so a set top
     * bit cannot turn into the high bits above the field.
     */
    inherit->flags = (unsigned long) (unsigned short) flags;
    CRASHCHECK(inherit->flags &
               SPAWN_SEARCH_PATH); /* posix spawn message always has a fully
                                      qualified path
                                      */

    if ((r = posix_spawnattr_getpgroup(attrp, &inherit->pgroup)) != EOK) {
        return r;
    }
    if ((r = posix_spawnattr_getsigmask(attrp, &inherit->sigmask)) != EOK) {
        return r;
    }
    if ((r = posix_spawnattr_getsigdefault(attrp, &inherit->sigdefault)) !=
        EOK) {
        return r;
    }
    sigemptyset(&inherit->sigignore);
    inherit->stack_max = 0;
    if ((r = posix_spawnattr_getschedpolicy(attrp, &inherit->policy)) != EOK) {
        return r;
    }
    inherit->runmask = ~0U;
    if ((r = posix_spawnattr_getschedparam(attrp, &inherit->param)) != EOK) {
        return r;
    }

    return EOK;
}

/* fact_list_get — the actions of a file-actions object as a heap array of *num
 * entries, which the caller frees. The sender sets the action count, so the
 * list is not put on the worker stack.
 *
 * Returns: EOK on success, otherwise an errno and *list is NULL.
 */
static int fact_list_get(posix_spawn_file_actions_t *factp,
                         posix_spawn_file_actions_list_t **list,
                         unsigned *num)
{
    int num_fact = 0;
    int r;

    *list = NULL;
    if ((r = rtmux_spawn_file_getactions(factp, &num_fact, NULL)) != EOK)
        return r;
    CRASHCHECK(num_fact > 0);
    num_fact = -num_fact;
    if (!(*list = calloc((size_t) num_fact + 1, sizeof(**list)))) {
        roottask_heap_note_nomem("spawn action list",
                                 ((size_t) num_fact + 1) * sizeof(**list));
        return ENOMEM;
    }
    if ((r = rtmux_spawn_file_getactions(factp, &num_fact, *list)) != EOK) {
        free(*list);
        *list = NULL;
        return r;
    }
    CRASHCHECK(num_fact < 0);
    *num = (unsigned) num_fact;
    return EOK;
}

/* Highest child fd an action targets, or -1 when the action is invalid. */
static int fact_target_fd(const posix_spawn_file_actions_list_t *fact)
{
    int fd;

    switch (fact->type) {
    case posix_file_action_type_CLOSE:
        fd = fact->_type.close.fd;
        break;
    case posix_file_action_type_DUP:
        if (fact->_type.dup.fd < 0)
            return -1;
        fd = fact->_type.dup.new_fd;
        break;
    case posix_file_action_type_OPEN:
        fd = fact->_type.open->new_fd;
        break;
    default:
        return -1;
    }
    return fd < 0 || fd > SPAWN_FD_TARGET_MAX ? -1 : fd;
}

/** calc_fdcnt
 *
 * Figure out from the file actions object, how many entries are required in the
 * 'fdmap' for the _proc_spawn message. It is not equivalent to the number of
 * file actions but rather to the highest index file action fd being
 * manipulated. This is one of the reasons that fdmap must eventually be
 * replaced by native POSIX file-action processing.
 *
 * Returns: EOK on success, otherwise an errno and *fdcnt should be considered
 * 			garbage.
 */
static int calc_fdcnt(posix_spawn_file_actions_t *factp, unsigned *fdcnt)
{
    posix_spawn_file_actions_list_t *fact_list;
    unsigned i, num_fact;
    int highest_fd = -1;
    int r;

    if ((r = fact_list_get(factp, &fact_list, &num_fact)) != EOK)
        return r;
    for (i = *fdcnt = 0; i < num_fact; i++) {
        int fd = fact_target_fd(&fact_list[i]);

        if (fd < 0) {
            r = EINVAL;
            break;
        }
        if (fd > highest_fd)
            highest_fd = fd;
    }
    free(fact_list);
    if (r == EOK)
        *fdcnt = (unsigned) max(highest_fd + 1, 3);
    return r;
}

/** add_fdmap
 *
 * Open actions are pre-opened by libc and arrive here as dup2() actions.
 *
 * Returns: EOK on success, otherwise errno and the fdmap should be considered
 * 			garbage
 */
static int add_fdmap(posix_spawn_file_actions_t *factp,
                     int32_t fdmap[],
                     unsigned fdcnt)
{
    posix_spawn_file_actions_list_t *fact_list;
    unsigned i, num_fact;
    int r;

    if ((r = fact_list_get(factp, &fact_list, &num_fact)) != EOK)
        return r;

    /* first, initialize entire fdmap to inherit */
    for (i = 0; i < fdcnt; i++) {
        fdmap[i] = (int32_t) i;
    }

    /* now process the specific file actions provided entering them at the
     * appropriate fdmap index
     */
    for (i = 0; i < num_fact && r == EOK; i++) {
        const posix_spawn_file_actions_list_t *fact = &fact_list[i];
        int fd, new_fd;

        switch (fact->type) {
        case posix_file_action_type_CLOSE:
            fd = fact->_type.close.fd;
            if (fd < 0 || fd >= (int) fdcnt)
                r = EINVAL;
            else
                fdmap[fd] = SPAWN_FDCLOSED;
            break;
        case posix_file_action_type_DUP:
            fd = fact->_type.dup.fd;
            new_fd = fact->_type.dup.new_fd;
            if (fd < 0 || new_fd < 0 || new_fd >= (int) fdcnt)
                r = EINVAL;
            else if (fd < (int) fdcnt && fdmap[fd] == SPAWN_FDCLOSED)
                r = EBADF;
            else if (fd != new_fd)
                fdmap[new_fd] = fd < (int) fdcnt ? fdmap[fd] : fd;
            break;
        default: /* open actions arrive here already opened, as dup2 */
            r = EINVAL;
            break;
        }
    }
    free(fact_list);
    return r;
}

/** parse_pspawn_msg (temporary)
 *
 * Create a proc_spawn_t message from a proc_posixspawn_t message
 *
 * The memory for the new proc_spawn_t message and the part_list is allocated.
 * It is the responsibility of the caller to free it
 */
static int parse_pspawn_msg(pid_t ppid,
                            proc_posixspawn_t *msg,
                            proc_spawn_t **spawn_msg)
{
    size_t spawn_msg_len;
    posix_spawnattr_t attrp = 0;
    posix_spawn_file_actions_t factp = 0;
    proc_spawn_t *new_msg;
    uint8_t *p, *p_new;
    int r;
    unsigned fdcnt = 0;
    unsigned fdmap_size = 0;
    posix_spawnattr_t *attrp_p = NULL;
    posix_spawn_file_actions_t *factp_p = NULL;

    CRASHCHECK(!msg);
    CRASHCHECK(!spawn_msg);

    *spawn_msg = NULL;

    /* if file actions have been provided we need to figure out the size of the
     * 'fdmap' for the _PROC_SPAWN message before its allocated. This size is
     * not what you think however. Because the 'fdmap' is index based, we must
     * allocate way more space than we really need. For example, if we only want
     * to modify file descriptor 10, we need fdmap array entries 0 through 9.
     */
    if (msg->i.filact_bytes && !msg->i.reserved[0]) {
        void *_factp = (void *) ((char *) (msg + 1) + msg->i.attr_bytes);
        void *open_action = (void *) ((char *) _factp + msg->i.filact_bytes);

        if (!pspawn_region_aligned(_factp))
            return EINVAL;

        /* fix any open file_actions pointers so they point to the local
         * messaged structure. All open actions are after the optional factp.
         * Pass the declared region sizes so the fixup bounds the num_entries
         * and open-record walks within the untrusted message.
         */
        if (file_open_actions_fixup(_factp, open_action, msg->i.filact_bytes,
                                    msg->i.filact_obytes) != EOK)
            return EINVAL;

        /* create the 'posix_spawn_file_actions_t' object */
        set_factp(&factp, _factp, (unsigned) key_mask_create(_factp));
        factp_p = (posix_spawn_file_actions_t *) obj_key_adj(&factp, _factp);

        if ((r = calc_fdcnt(&factp, &fdcnt)) != EOK) {
            return r;
        }
        if (fdcnt > UINT16_MAX ||
            fdcnt > SIZE_MAX / sizeof(/*msg->i.fdmap[0] */ int32_t)) {
            return E2BIG;
        }
        fdmap_size = fdcnt * sizeof(/*msg->i.fdmap[0] */ int32_t);
    }

    /* the new spawn message will have the following optional extensions layout
     * as per procmsg.h ...
     *     - an fdmap built from the file actions structure (fdcnt *
     * sizeof(int32_t))
     *     - no search - searchlen = 0 (because posix_spawn() will already have
     *       provided a fully qualified path to the executable)
     *     - a path and length equivalent to msg->i.pathlen (same reason as
     * above)
     *     - identical argv and env parameters (nbytes == argenv_bytes)
     */
    spawn_msg_len = sizeof(new_msg->i);
    if (spawn_add_size(&spawn_msg_len, fdmap_size) != EOK ||
        spawn_add_size(&spawn_msg_len, msg->i.cwdlen) != EOK ||
        spawn_add_size(&spawn_msg_len, msg->i.pathlen) != EOK ||
        spawn_add_size(&spawn_msg_len, msg->i.argenv_bytes) != EOK)
        return E2BIG;
    if (spawn_msg_len > (size_t) INT_MAX)
        return E2BIG;
    if (!(new_msg = calloc(1, spawn_msg_len))) {
        roottask_heap_note_nomem("spawn message", spawn_msg_len);
        return ENOMEM;
    }

    new_msg->i.type = _PROC_SPAWN;
    new_msg->i.subtype = msg->i.subtype;
    new_msg->i.searchlen = 0;
    new_msg->i.pathlen = msg->i.pathlen;
    new_msg->i.nfds = (uint16_t) fdcnt;
    new_msg->i.nargv = msg->i.nargv;
    new_msg->i.narge = msg->i.narge;
    new_msg->i.nbytes = msg->i.argenv_bytes;
    new_msg->i.cwdlen = msg->i.cwdlen;

    /* now, add the 'fdmap' in the new message as required */
    if (factp_p) {
        if ((r = add_fdmap(factp_p, (int32_t *) (new_msg + 1), fdcnt)) != EOK) {
            free(new_msg);
            return r;
        }
    }

    /* put in the path to the executable */
    CRASHCHECK(fdmap_size !=
               (new_msg->i.nfds * sizeof(/*msg->i.fdmap[0] */ int32_t)));
    p_new = (uint8_t *) ((char *) &new_msg->i.nbytes +
                         sizeof(new_msg->i.nbytes) + fdmap_size);
    p = (uint8_t *) ((char *) (msg + 1) + msg->i.attr_bytes +
                     msg->i.filact_bytes + msg->i.filact_obytes);

    /* the child cwd trails the posix message but precedes the path here */
    memcpy(p_new, p + msg->i.pathlen + msg->i.argenv_bytes, msg->i.cwdlen);
    p_new += msg->i.cwdlen;
    memcpy(p_new, p, new_msg->i.pathlen);

    /* add in remaining args and env */
    p_new += new_msg->i.pathlen;
    p += new_msg->i.pathlen;
    memcpy(p_new, p, new_msg->i.nbytes);

    /* create a valid 'posix_spawnattr_t' object. This allows us to use the
     * posix_spawnattr_getxxx() functions to extract the necessary info. Reject
     * an attr region too small for the object first, so the getters below do
     * not read past it (into the following in-message regions).
     */
    if (!rtmux_spawnattr_msg_ok(msg->i.attr_bytes)) {
        free(new_msg);
        return EINVAL;
    }
    p = (void *) (msg + 1);
    set_attrp(&attrp, p,
              (unsigned) key_mask_create(
                  p)); /* create the 'posix_spawnattr_t' object */

    attrp_p = (posix_spawnattr_t *) obj_key_adj(&attrp, p);

    if ((r = fill_struct_inheritance(&new_msg->i.parms, attrp_p)) != EOK) {
        free(new_msg);
        return r;
    }
    new_msg->i.parms.flags |= SPAWN_FD_SCAN_TAIL;
    *spawn_msg = new_msg;
    return EOK;
}

/** proc_pspawn
 *
 * Implement posix_spawn(): receive a _PROC_POSIX_SPAWN message and rebuild it
 * as a _PROC_SPAWN with parse_pspawn_msg(). A message longer than the receive
 * buffer (large argv/envp) is first read whole into a heap copy, up to
 * PSPAWN_MSG_MAX, so the rebuilt message carries every byte and its loader
 * never reads the sender's buffer, whose layout differs.
 */
int proc_pspawn(ioserv_context_t *ctp, void *vmsg)
{
    proc_posixspawn_t *msg = vmsg;
    proc_spawn_t *spawn_msg = NULL;
    void *whole = NULL;
    uint64_t len;
    int r;

    /* The client declares the sizes of the attr / file-action / path / argenv
     * sub-regions that follow the header. parse_pspawn_msg() uses them for
     * pointer arithmetic into the message, so reject a message whose declared
     * regions are not all in it before any of them is walked. Floor-check the
     * header first: the size fields below live in the header, so msglen must
     * cover it before they are read.
     */
    if (ctp->info.msglen < sizeof(*msg))
        return EMSGSIZE;
    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
        return ENOTSUP; /* before the header's fields are believed */
    /* ARG_MAX first: an oversized argument list is E2BIG whether or not the
     * sender went on to send it, and nothing is allocated for it.
     */
    if (!spawn_args_fit(msg->i.argenv_bytes, msg->i.nargv, msg->i.narge))
        return E2BIG;
    len = pspawn_declared_len(&msg->i);
    if (len > ctp->info.srcmsglen)
        return EMSGSIZE;
    if (len > PSPAWN_MSG_MAX)
        return E2BIG;
    if (msg->i.reserved[1] || msg->i.reserved[0] > PSPAWN_CHILD_ACTIONS ||
        (msg->i.reserved[0] && (msg->i.cwdlen || msg->i.filact_obytes)))
        return EINVAL;
    if (msg->i.subtype != _PROC_SPAWN_START)
        return ENOSYS;

    if (len > ctp->info.msglen) {
        int head = (int) ctp->info.msglen, tail;

        tail = (int) len - head;
        if (!(whole = malloc((size_t) len))) {
            roottask_heap_note_nomem("spawn request", (size_t) len);
            return ENOMEM;
        }

        /* The checked header comes from the receive buffer; only the tail is
         * read from the sender.
         */
        memcpy(whole, msg, (size_t) head);
        if (msg_read(ctp->rcvid, (char *) whole + head, tail, head) != tail) {
            free(whole);
            return EFAULT;
        }
        msg = whole;
    }

    const char *path = (const char *) (msg + 1) + msg->i.attr_bytes +
                       msg->i.filact_bytes + msg->i.filact_obytes;
    if (!msg->i.pathlen || memchr(path, 0, msg->i.pathlen) != path + msg->i.pathlen - 1)
        r = EINVAL;
    else
        r = parse_pspawn_msg(ctp->info.pid, msg, &spawn_msg);

    if (r == EOK) {
        size_t spawn_msg_len = sizeof(*spawn_msg) + spawn_msg->i.searchlen +
                               spawn_msg->i.cwdlen + spawn_msg->i.pathlen +
                               spawn_msg->i.nfds * sizeof(int32_t) +
                               spawn_msg->i.nbytes;
        ioserv_context_t ctp2 = *ctp;

        ctp2.info.flags &= (uint16_t) ~RTMUX_MI_ENDIAN_DIFF;
        ctp2.msg = (ioserv_msg_t *) spawn_msg;
        ctp2.info.srcmsglen = (uint32_t) spawn_msg_len;
        ctp2.info.msglen = (uint32_t) spawn_msg_len;

        CRASHCHECK(ctp2.info.srcmsglen > ctp2.info.msglen);
        const unsigned char *actions = (const unsigned char *) (msg + 1) + msg->i.attr_bytes;
        r = proc_spawn_with_actions(&ctp2, spawn_msg, actions,
                                    msg->i.reserved[0] ? msg->i.filact_bytes : 0);
    }
    free(whole);
    free(spawn_msg);
    return r;
}
