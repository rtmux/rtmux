/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include <alloca.h>
#include <limits.h>
#include <spawn.h>
#include <sys/posix-spawn.h>

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

static int pspawn_add_size(size_t *sum, size_t add)
{
    if (add > SIZE_MAX - *sum)
        return E2BIG;
    *sum += add;
    return EOK;
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
    inherit->flags = (unsigned long) flags;
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
    int num_fact, highest_fd;
    posix_spawn_file_actions_list_t *fact_list;
    unsigned i;
    int r;

    /* determine how much space to leave for fdmap entries */
    num_fact = 0;
    if ((r = rtmux_spawn_file_getactions(factp, &num_fact, NULL)) != EOK)
        return r;
    CRASHCHECK(num_fact > 0);
    num_fact = -num_fact;
    if (!(fact_list = alloca((size_t) num_fact * sizeof(*fact_list))))
        return ENOMEM;
    if ((r = rtmux_spawn_file_getactions(factp, &num_fact, fact_list)) != EOK)
        return r;
    CRASHCHECK(num_fact < 0);

    highest_fd = -1;

    /* now run the actions list looking for the highest 'fd' to be operated on
     */
    for (i = *fdcnt = 0; i < num_fact; i++) {
        int fd = -1;
        switch (fact_list[i].type) {
        case posix_file_action_type_CLOSE:
            fd = fact_list[i]._type.close.fd;
            if (fd < 0 || fd > SPAWN_FD_TARGET_MAX)
                return EINVAL;
            break;
        case posix_file_action_type_DUP:
            if (fact_list[i]._type.dup.fd < 0 ||
                fact_list[i]._type.dup.new_fd < 0 ||
                fact_list[i]._type.dup.new_fd > SPAWN_FD_TARGET_MAX)
                return EINVAL;
            fd = fact_list[i]._type.dup.new_fd;
            break;
        case posix_file_action_type_OPEN:
            fd = fact_list[i]._type.open->new_fd;
            if (fd < 0 || fd > SPAWN_FD_TARGET_MAX)
                return EINVAL;
            break;
        default:
            return EINVAL;
        }
        if (fd > highest_fd)
            highest_fd = fd;
    }
    *fdcnt = (unsigned) max(highest_fd + 1, 3);
    return EOK;
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
    int r, num_fact = 0;
    unsigned i;

    num_fact = 0;
    if ((r = rtmux_spawn_file_getactions(factp, &num_fact, NULL)) != EOK)
        return r;
    CRASHCHECK(num_fact > 0);
    num_fact = -num_fact;
    if (!(fact_list = alloca((size_t) num_fact * sizeof(*fact_list))))
        return ENOMEM;
    if ((r = rtmux_spawn_file_getactions(factp, &num_fact, fact_list)) != EOK)
        return r;
    CRASHCHECK(num_fact < 0);

    /* first, initialize entire fdmap to inherit */
    for (i = 0; i < fdcnt; i++) {
        fdmap[i] = (int32_t) i;
    }

    /* now process the specific file actions provided entering them at the
     * appropriate fdmap index
     */
    for (i = 0; i < num_fact; i++) {
        switch (fact_list[i].type) {
        case posix_file_action_type_CLOSE:
            if (fact_list[i]._type.close.fd < 0 ||
                fact_list[i]._type.close.fd >= (int) fdcnt)
                return EINVAL;
            fdmap[fact_list[i]._type.close.fd] = SPAWN_FDCLOSED;
            break;
        case posix_file_action_type_DUP:
            if (fact_list[i]._type.dup.fd < 0 ||
                fact_list[i]._type.dup.new_fd < 0 ||
                fact_list[i]._type.dup.new_fd >= (int) fdcnt)
                return EINVAL;
            if (fact_list[i]._type.dup.fd < (int) fdcnt &&
                fdmap[fact_list[i]._type.dup.fd] == SPAWN_FDCLOSED)
                return EBADF;
            if (fact_list[i]._type.dup.fd == fact_list[i]._type.dup.new_fd)
                break;
            fdmap[fact_list[i]._type.dup.new_fd] =
                fact_list[i]._type.dup.fd < (int) fdcnt
                    ? fdmap[fact_list[i]._type.dup.fd]
                    : fact_list[i]._type.dup.fd;
            break;
        case posix_file_action_type_OPEN:
            return EINVAL;
        default:
            return EINVAL;
        }
    }
    return EOK;
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
    if (msg->i.filact_bytes > 0) {
        void *_factp = (void *) ((char *) (msg + 1) + msg->i.attr_bytes);
        void *open_action = (void *) ((char *) _factp + msg->i.filact_bytes);

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
    if (pspawn_add_size(&spawn_msg_len, fdmap_size) != EOK ||
        pspawn_add_size(&spawn_msg_len, msg->i.pathlen) != EOK ||
        pspawn_add_size(&spawn_msg_len, msg->i.argenv_bytes) != EOK)
        return E2BIG;
    if (spawn_msg_len > (size_t) INT_MAX)
        return E2BIG;
    if (!(new_msg = calloc(1, spawn_msg_len))) {
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
 * Implement posix_spawn() Currently we receive a _PROC_POSIX_SPAWN message and
 * then reformat it into a _PROC_SPAWN message with parse_pspawn_msg(). See
 * rationale comment below
 */
int proc_pspawn(ioserv_context_t *ctp, void *vmsg)
{
    proc_posixspawn_t *msg = vmsg;
    proc_spawn_t *spawn_msg = NULL;

    /* For now, we will not handle multipart messages. If all of the
     * proc_posixspawn_t message could not be sent, none will be processed
     */
    if (ctp->info.srcmsglen > ctp->info.msglen)
        return EMSGSIZE;

    /* The client declares the sizes of the attr / file-action / path / argenv
     * sub-regions that follow the header. parse_pspawn_msg() uses them for
     * pointer arithmetic into the received buffer, so reject a message whose
     * declared regions do not fit within msglen before any of them is walked.
     * Floor-check the header first: the size fields below live in the header,
     * so msglen must cover it before they are read. 64-bit sum: the uint32 byte
     * counts could otherwise overflow.
     */
    if (ctp->info.msglen < sizeof(*msg))
        return EMSGSIZE;
    if ((uint64_t) sizeof(*msg) + msg->i.attr_bytes + msg->i.filact_bytes +
            msg->i.filact_obytes + msg->i.pathlen + msg->i.argenv_bytes >
        ctp->info.msglen)
        return EMSGSIZE;

    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
        return ENOTSUP;

    switch (msg->i.subtype) {
    case _PROC_SPAWN_START: {
        int r;

        if ((r = parse_pspawn_msg(ctp->info.pid, msg, &spawn_msg)) == EOK) {
            size_t spawn_msg_len = sizeof(*spawn_msg) + spawn_msg->i.searchlen +
                                   spawn_msg->i.pathlen +
                                   spawn_msg->i.nfds * sizeof(int32_t) +
                                   spawn_msg->i.nbytes;

            ioserv_context_t ctp2 = *ctp;

            ctp2.info.flags &= (uint16_t) ~RTMUX_MI_ENDIAN_DIFF;
            ctp2.msg = (ioserv_msg_t *) spawn_msg;
            ctp2.info.srcmsglen = (uint32_t) spawn_msg_len;
            ctp2.info.msglen = (uint32_t) spawn_msg_len;

            CRASHCHECK(ctp2.info.srcmsglen > ctp2.info.msglen);
            r = proc_spawn(&ctp2, spawn_msg);
        }
        if (spawn_msg) {
            free(spawn_msg);
        }
        return r;
    }
    default:
        return ENOSYS;
    }
}
