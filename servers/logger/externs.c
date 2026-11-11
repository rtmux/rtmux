/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"

int Verbose;
int NumInts;
int LogFflags;
int LogFsize;
char *LogFname;
int FilterLog;
int TeeStdout;
bool AttachConsole = true;
bool CrashOnWrite;
dispatch_t *LoggerDpp;
const char *logger_program_name = "logger";
