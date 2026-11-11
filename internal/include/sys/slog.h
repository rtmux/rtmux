/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/slog.h RTMux system logging */
#ifndef __SLOG_H_INCLUDED
#define __SLOG_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

#define _SLOG_SHUTDOWN 0 /* Shut down the system NOW. eg: for OEM use */
#define _SLOG_CRITICAL \
    1 /* Unexpected unrecoverable error. eg: hard disk error */
#define _SLOG_ERROR \
    2 /* Unexpected recoverable error. eg: needed to reset a hw controller */
#define _SLOG_WARNING                                                       \
    3                  /* Expected error. eg: parity error on a serial port \
                        */
#define _SLOG_NOTICE 4 /* Warnings. eg: Out of paper */
#define _SLOG_INFO 5   /* Information. eg: Printing page 3 */
#define _SLOG_DEBUG1 6 /* Debug messages eg: Normal detail */
#define _SLOG_DEBUG2 7 /* Debug messages eg: Fine detail */

#define _SLOG_SEVMAXVAL 0x7 /* The largest number for severity field */


/* Each log event contains an opcode field followed by a time field in seconds
 * and a count and severity and msec field. Data follows... Count is in 32 bit
 * words.
 *
 *          cccccccc   mmmmmm mmmmtsss   [c]ount,[m]sec,[t]ext,[s]everity
 * mmmmmmmm mmmmMMMM MMMMMMMM MMMMMMMM [m]inor,[M]ajor SSSSSSSS SSSSSSSS
 * SSSSSSSS SSSSSSSS [S]econds
 */
#define _SLOG_HDRINTS 3
#define _SLOG_GETSEC(op) (op)
#define _SLOG_GETCOUNT(op) (((op) >> 16) & 0xff)
#define _SLOG_GETTEXT(op) ((op) & 0x8)
#define _SLOG_TEXTBIT 0x8
#define _SLOG_GETSEVERITY(op) ((op) & 0x7)
#define _SLOG_GETMINOR(op) (((op) >> 20) & 0xfff)
#define _SLOG_GETMAJOR(op) ((op) & 0xfffff)
#define _SLOG_SETCODE(ma, mi) ((ma) | ((mi) << 20))

#define _SLOG_MAXSIZE \
    ((0xff + 3 - 1) * sizeof(int)) /* Maximum event size in bytes */

#endif
