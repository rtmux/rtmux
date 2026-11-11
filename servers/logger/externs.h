/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SERVERS_LOGGER_EXTERNS_H
#define SERVERS_LOGGER_EXTERNS_H

struct slogdev;
struct slog_ocb;
#define IOSERV_HANDLE_T struct slogdev
#define IOSERV_OCB_T struct slog_ocb

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/ioserv.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/path-registry.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/slog.h>
#include "struct.h"
#include "service.h"

extern int Verbose;
extern int NumInts;
extern int LogFflags;
extern int LogFsize;
extern char *LogFname;
extern int FilterLog;
extern int TeeStdout;
extern bool AttachConsole;
extern bool CrashOnWrite;
extern dispatch_t *LoggerDpp;
extern const char *logger_program_name;

#endif
