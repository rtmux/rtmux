/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_SPAWN_LIMITS_H_INCLUDED
#define RTMUX_SPAWN_LIMITS_H_INCLUDED

/* The size limits of a spawn request, shared by libc, which refuses an
 * oversized request before sending it, and roottask, which refuses it again.
 *
 * ARG_MAX (<limits.h>) bounds what the new program is given: every argument and
 * environment string with its NUL, plus one pointer for each of them and for
 * the null pointer that ends each of the two vectors. spawn_args_fit() is the
 * one statement of that rule. Both spawn requests and so every exec function
 * are held to it.
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/procmsg.h>

/* Largest _PROC_POSIX_SPAWN, which roottask copies whole into its heap: the
 * arguments, and room for the attributes, the path and the file actions.
 */
#define PSPAWN_MSG_MAX (256U * 1024U)

/* Largest _PROC_SPAWN: the arguments after a descriptor map, a search path, a
 * working directory and a path, each as long as its 16-bit count allows.
 */
#define SPAWN_MSG_MAX (576U * 1024U)

_Static_assert(ARG_MAX >= _POSIX_ARG_MAX, "ARG_MAX is below the POSIX minimum");
_Static_assert(sizeof(struct _proc_spawn) + UINT16_MAX * sizeof(int32_t) +
                       3U * UINT16_MAX + ARG_MAX <=
                   SPAWN_MSG_MAX,
               "_PROC_SPAWN: header, full fd map, search, cwd, path and "
               "ARG_MAX of arguments must fit SPAWN_MSG_MAX");
_Static_assert(sizeof(struct _proc_posix_spawn) + PATH_MAX + 1 + ARG_MAX <
                   PSPAWN_MSG_MAX,
               "_PROC_POSIX_SPAWN: header, path and ARG_MAX of arguments must "
               "fit PSPAWN_MSG_MAX with room for attributes and file actions");
/* Every counted entry costs a pointer and a NUL, so both counts fit 16 bits. */
_Static_assert(ARG_MAX / (sizeof(char *) + 1) <= UINT16_MAX,
               "ARG_MAX admits more strings than the wire counts hold");

/* Bytes of the two pointer vectors, with their terminating null pointers. */
static inline size_t spawn_arg_pointers(unsigned argc, unsigned envc)
{
    return ((size_t) argc + envc + 2) * sizeof(char *);
}

/* True when `strings` bytes of argument and environment strings (NULs
 * included) in argc + envc strings are within ARG_MAX.
 */
static inline int spawn_args_fit(size_t strings, unsigned argc, unsigned envc)
{
    return strings <= ARG_MAX &&
           spawn_arg_pointers(argc, envc) <= ARG_MAX - strings;
}

/* Bytes of a whole _PROC_POSIX_SPAWN as its header declares them, in 64 bits:
 * the sum of the 32-bit counts cannot wrap.
 */
static inline uint64_t pspawn_declared_len(const struct _proc_posix_spawn *i)
{
    return (uint64_t) sizeof(*i) + i->attr_bytes + i->filact_bytes +
           i->filact_obytes + i->pathlen + i->argenv_bytes + i->cwdlen;
}

#endif
