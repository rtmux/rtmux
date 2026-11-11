/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_WAIT_OPS_H_INCLUDED
#define _SYS_WAIT_OPS_H_INCLUDED

/* Private waitid option used by libc waitpid() and roottask's wait service to
 * include ptrace trap stops in waitpid() results without exposing a non-POSIX
 * WTRAPPED flag from <sys/wait.h>.
 */
#define RTMUX_WTRAPPED 0x0002

#endif
