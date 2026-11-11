/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DEVCTL_TTY_H_INCLUDED
#define __DEVCTL_TTY_H_INCLUDED

#ifndef _DEVCTL_H_INCLUDED
#include <devctl.h>
#endif

/* Where possible we want the devctl to match the ioctl exactly */
#define _CMD_IOCTL_TTY 't'

/* Desc: This call is made to get the current terminal control settings of a
 * device. Args: See the tcgettattr documentation. Notes: This function is for
 * internal use and should not be called directly. Instead use the tcgettattr
 * cover function.
 */
#define DEVCTL_TTY_GET_ATTR \
    __DIOF(_CMD_IOCTL_TTY, 19, struct termios) /* TIOCGETA */

/* Desc: These calls are made to change the current terminal control settings of
 * a device. Args: See the tcsettattr documentation. Notes: This function is for
 * internal use and should not be called directly. Instead use the tcsettattr
 * cover function.
 */
#define DEVCTL_TTY_SET_ATTR \
    __DIOT(_CMD_IOCTL_TTY, 20, struct termios) /* TIOCSETA */
#define DEVCTL_TTY_SET_ATTR_DRAIN \
    __DIOT(_CMD_IOCTL_TTY, 21, struct termios) /* TIOCSETAW */
#define DEVCTL_TTY_SET_ATTR_FLUSH \
    __DIOT(_CMD_IOCTL_TTY, 22, struct termios) /* TIOCSETAF */

/* Desc: This call discards data received from a device but not yet read, per
 * the queue selector. Args: an int queue selector (TCIFLUSH/TCOFLUSH/
 * TCIOFLUSH). Notes: This function is for internal use and should not be called
 * directly. Instead use the tcflush cover function.
 */
#define DEVCTL_TTY_FLUSH __DIOT(_CMD_IOCTL_TTY, 16, int) /* TIOCFLUSH */

/* Desc: This call blocks until all output written to a device has been
 * transmitted. Args: none. Notes: This function is for internal use and should
 * not be called directly. Instead use the tcdrain cover function.
 */
#define DEVCTL_TTY_DRAIN __DION(_CMD_IOCTL_TTY, 94) /* TIOCDRAIN */

/* Desc: This call sends a break (a stream of zero-valued bits) on an
 * asynchronous serial line; on a terminal that is not an async serial line it
 * does nothing. Args: none — the break length is implementation-defined. Notes:
 * This function is for internal use and should not be called directly. Instead
 * use the tcsendbreak cover function.
 */
#define DEVCTL_TTY_SENDBREAK __DION(_CMD_IOCTL_TTY, 93) /* TIOCSBRK-like */

/* Desc: This call performs terminal flow-control actions. Args: an int action
 * (TCOOFF/TCOON/TCIOFF/TCION). Notes: This function is for internal use and
 * should not be called directly. Instead use the tcflow cover function.
 */
#define DEVCTL_TTY_FLOW __DIOT(_CMD_IOCTL_TTY, 95, int)

/* Desc: This call is made to get the process group ID associated with a device.
 * Args: See the tcgetpgrp documentation. Notes: This function is for internal
 * use and should not be called directly. Instead use the tcgetpgrp cover
 * function.
 */
#define DEVCTL_TTY_GET_PGRP __DIOF(_CMD_IOCTL_TTY, 119, pid_t) /* TIOCGPGRP */

/* Desc: This call is made to set the process group ID associated with a device.
 * Args: See the tcsetpgrp documentation. Notes: This function is for internal
 * use and should not be called directly. Instead use the tcsetpgrp cover
 * function.
 */
#define DEVCTL_TTY_SET_PGRP __DIOT(_CMD_IOCTL_TTY, 118, pid_t) /* TIOCSPGRP */

/* Desc: This call is made set the process group ID of the session leader for a
 * controlling terminal. Args: See the tcsetsid documentation. Notes: This
 * function is for internal use and should not be called directly. Instead use
 * the tcsetsid cover function.
 */
#define DEVCTL_TTY_SET_SID __DIOT(DEVCTL_CLASS_TTY, 8, pid_t)

/* Desc: This call is made to test to see if a file descriptor is associated
 * with a terminal. Args: See the isatty documentation. Notes: This function is
 * for internal use and should not be called directly. Instead use the isatty
 * cover function.
 */
#define DEVCTL_TTY_ISATTY __DION(DEVCTL_CLASS_TTY, 24)

/* Desc: This call is made to determine the number of bytes waiting to be read.
 * Notes: Internal byte-count query used by pipe and character-device handlers.
 */
#define DEVCTL_TTY_GET_INPUT_COUNT __DIOF('f', 127, unsigned) /* FIONREAD */

#endif
