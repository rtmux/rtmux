/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <spawn.h>
#include <sys/posix-spawn.h>

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <endian.h>
#include <sys/cdefs.h>

#include <assert.h>

/* AKEY
 *
 * see set_attrp()/get_attrp for description of use
 */
#define AKEY(_km_) (0x031759f0 | (_km_))

/* GET_ATTRP SET_ATTRP
 *
 * These macros wrap the get_attrp()/set_attrp() routines. For set we always use
 * KEY_MASK to enforce 8 byte alignment within this library however the get
 * operation will inspect the lower MIN_KEY_MASK bits of the incoming
 * 'posix_spawnattr_t' object pointer and if non zero, will extract those bits
 * for use as the key mask established during a set operation for the object.
 * This allows proc service to use an alternate alignment constraint when
 * processing the _PROC_POSIX_SPAWN message. All user programs will however be
 * forced to the alignment established by KEY_MASK.
 */
#define SET_ATTRP(a, b) set_attrp((a), (b), KEY_MASK)
#define GET_ATTRP(a)                                                    \
    get_attrp(*(posix_spawnattr_t *) ((uintptr_t) (a) & ~MIN_KEY_MASK), \
              ((uintptr_t) (a) & MIN_KEY_MASK)                          \
                  ? (uintptr_t) (a) & MIN_KEY_MASK                      \
                  : KEY_MASK)

/* _posix_spawnattr_t
 *
 * Internal representation of the opaque posix_spawnattr_t. A posix_spawnattr_t
 * is simply a 'void *'. The static initializer POSIX_SPAWNATTR_INIT as well as
 * the posix_spawnattr_init() function will set the contents of the user
 * allocated posix_spawnattr_t object to NULL. An attributes object initialized
 * this way will have the same effect as NULL for the posix_spawnattr_t
 * parameter to posix_spawn() ... that is to use default attributes.
 *
 * Setting non default values for the posix_spawnattr_t object will cause memory
 * to be allocated on the very first _setxxx() call. The address of this
 * allocated memory will be stuffed into the caller provided posix_spawnattr_t
 * object for use in other set calls. This has the effect of making the
 * 'posix_spawnattr_t' object completely opaque to user application as well as
 * requiring only 4 bytes for the 'posix_spawnattr_t' object if default values
 * will be used.
 */
typedef struct {
    /* the following fields are POSIX mandated */
    uint32_t flags;
    pid_t pgrp; /* POSIX */

    struct {
        sigset_t dflt; /* POSIX */
        sigset_t mask; /* POSIX */
    } sig;

    struct {
        struct sched_param param; /* POSIX */
        int32_t policy;           /* POSIX */
    } sched;

} _posix_spawnattr_t;


/* internal prototypes */
extern const _posix_spawnattr_t _default_posix_spawnattr_t __hidden;

extern _posix_spawnattr_t *_posix_spawnattr_t_realloc(_posix_spawnattr_t *attrp,
                                                      size_t size) __hidden;
extern _posix_spawnattr_t *get_attrp(uintptr_t x,
                                     unsigned int key_mask) __hidden;
extern unsigned valid_attrp(const posix_spawnattr_t *__restrict attrp) __hidden;
extern void set_attrp(posix_spawnattr_t *pp,
                      void *_p,
                      unsigned key_mask) __hidden;

static inline int _posix_spawnattr_for_set(posix_spawnattr_t *attrp,
                                           _posix_spawnattr_t **out)
{
    _posix_spawnattr_t *_attrp;

    if (!valid_attrp(attrp)) {
        return EINVAL;
    }

    _attrp = GET_ATTRP(attrp);
    if (!_attrp) {
        _attrp = _posix_spawnattr_t_realloc(NULL, 0);
        if (!_attrp) {
            return ENOMEM;
        }
        SET_ATTRP(attrp, _attrp);
    }
    *out = _attrp;
    return EOK;
}
