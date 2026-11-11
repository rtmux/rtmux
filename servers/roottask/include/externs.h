/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ROOTTASK_EXTERNS_H
#define _ROOTTASK_EXTERNS_H

/* Project-wide Kconfig macros (CONFIG_PROCESSORS_MAX, CONFIG_SMP,
 * CONFIG_PA_BITS, etc.). Without this, every CONFIG_* in roottask code silently
 * expands to 0 — e.g. process_shutdown's SMP wait loop guarded by "#if
 * CONFIG_PROCESSORS_MAX > 1" would compile out, while the same source compiled
 * into the kernel sees the macro and includes the loop. Mirrors
 * kernel/include/config.h's role on the kernel side.
 */
#include <generated/autoconf.h>

#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <atomic.h>
#include <endian.h>
#include <minmax.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/syspage.h>
#include <sys/node.h>

#undef THREAD_POOL_PARAM_T
#define THREAD_POOL_PARAM_T dispatch_context_t
#include <sys/ioserv.h>
#include <sys/iomsg.h>

#include <arch/cpu-def.h>

#ifndef KERNEL_HDR
#include <objects.h>
#include <variables.h>
#include <paging.h>
#endif

#include "struct.h"

/* The kernel's full declaration header (decls.h) takes pointers to
 * kernel-private types (process_t, thread_t, connect_t, ...) which U-mode
 * roottask cannot dereference, so it is gated behind KERNEL_INTERNAL. Roottask
 * gets only the public utility declarations (souls + vectors) via
 * <private/kernel-util.h>.
 */
#include <private/kernel-util.h>
#include "roottask.h"
#include "process.h"
#include "namesrv.h"
#include "path.h"
#include "vm.h"

#undef EXT
#undef INIT1
#undef INIT2
#undef INIT7
#undef INITSOUL

#ifndef PROCHDR
#define EXT extern
#define INIT1(a)
#define INIT2(a, b)
#define INIT7(a, b, c, d, e, f, g)
#define INITSOUL(a, b, c, d, e)
#else
#define EXT
#define INIT1(a) = a
#define INIT2(a, b) = {a, b}
#define INIT7(a, b, c, d, e, f, g) = {a, b, c, d, e, f, g}
#define INITSOUL(a, b, c, d, e) = {0, a, 0, 0, sizeof(b), 0, c, c}
#endif

/* Roottask's own process_t pointer. Owned by roottask, set up at startup (see
 * roottask_main). Roottask runs as its own pid 1 with its own as, so this is
 * the act->process at startup.
 */
EXT process_t *roottask_prp;

/* Roottask log level: 0 = info-and-above only, 1 = +debug, 2 = +trace. Set by
 * roottask_main from "-v" / "-vv" / "-vvv" arguments. Read by root_log's
 * console-echo branch to filter root_dbg/root_trace output.
 */
EXT int roottask_loglevel;

/* Boot-time tunables previously read from the kernel's "kopt" struct. "kopt" is
 * now strictly kernel-internal; roottask keeps its own copies, parsed from argv
 * by roottask_main using the same "-F"/"-u" flags the kernel parses. Defaults
 * match kernel/include/options.h's KERNEL_OPTIONS_INIT (max_fds=1000,
 * procfs_umask=0022).
 */
EXT unsigned roottask_max_fds INIT1(1000);
EXT mode_t roottask_procfs_umask INIT1(0022);

/* VM operation table — roottask-resident storage. The kernel uses its own
 * resident table.
 */
EXT vm_ops_t root_vm_ops;

EXT int root_id;
EXT int link_root_id;

/* mem_free_size moved to kernel/ext/pa.c — roottask reads it via pa_free_size()
 * which calls __KSYS_PRIV_PA_GET_SIZES.
 */
EXT dispatch_t *dpp;

void *_smalloc(size_t size);
void *_scalloc(size_t size);
void *_srealloc(void *data, size_t old_size, size_t new_size);
void _sfree(void *data, size_t size);

EXT dev_t path_devno;

/* Device number of the read-only boot cpio (see cpiofs.c). */
dev_t cpiofs_boot_devno(void);

extern int proc_service_scoid;

/* Build the _IO_DUP a spawn loader sends so a child inherits a parent's fd. The
 * _PROC_SPAWN_FD handler keys exactly these bytes (up to i.key), so the loader
 * and the handler must both build them here.
 */
static inline void spawn_dup_init(io_dup_t *d,
                                  pid_t ppid,
                                  uint32_t srcnd,
                                  int32_t scoid,
                                  int32_t coid)
{
    memset(d, 0, sizeof *d);
    d->i.type = _IO_DUP;
    d->i.combine_len = sizeof d->i;
    d->i.info.nd = srcnd;
    d->i.info.pid = ppid;
    d->i.info.scoid = scoid;
    d->i.info.coid = coid;
}

struct loader_startup;
EXT int (*elf_load_hook)(int fd,
                         const char *path,
                         struct loader_startup *lsp,
                         struct stat *statlocal,
                         struct inheritance *parms);

/* These two variables automatically created every link in the timestamp.c file.
 */
extern const char timestamp[];
extern const char os_version_string[];

#undef INITSOUL
#define INITSOUL(a, b, c, d, e) = {0, a, 0, 0, sizeof(b), 0, c, c}

#ifndef INT_STRLEN_MAXIMUM

/* 302 / 1000 is log10(2.0) rounded up. Subtract one for the sign bit if the
 * type is signed; add one for integer division truncation; add one more for a
 * minus sign if the type is signed.
 */
#define INT_STRLEN_MAXIMUM(type)                                         \
    (((sizeof(type) * CHAR_BIT) - (((type) - 1) < 0)) * 302 / 1000 + 1 + \
     (((type) - 1) < 0))
#endif

/* Private call slots (__KSYS_*) + PRIV sub-namespace (__KSYS_PRIV_*). The
 * latter lives in kernel/include/private/priv-calls.h.
 */
#include <sys/calls.h>
#include <private/priv-calls.h>

/* Defined in servers/roottask/proc/kext-wrappers.c. Re-declared here because
 * the matching prototype lives in kernel/include/priv.h, which is kernel-
 * internal and unreachable from roottask.
 */
void signal_kill(channel_t *chp, int signo, pid_t sender_pid);

/* DBGOUT — now in <kernel/log.h> */
#include <log.h>

/* Roottask logging subsystem (root_log/root_err/root_warn/root_dbg/root_trace)
 */
#include "root-log.h"

#endif
