/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef DYNLINK_PRINTF_H
#define DYNLINK_PRINTF_H 1

#include <sys/cdefs.h>
#include <stdarg.h>
#include <unistd.h>

int ldso_snprintf(char *buf, size_t bufsize, const char *fmt, ...)
    __printflike(3, 4);
int ldso_vsnprintf(char *buf, size_t bufsize, const char *fmt, va_list ap);
int ldso_vfdprintf(int fd, const char *fmt, va_list ap);
int ldso_fdprintf(int fd, const char *fmt, ...) __printflike(2, 3);
int ldso_fdprintfx(int fd, const char *fmt, ...);
void ldso_fdputstr(int fd, const char *str);
void ldso_fdputchar(int fd, int c);

#define ldso_printf(...) ldso_fdprintf(STDOUT_FILENO, __VA_ARGS__)
#define ldso_putstr(str) ldso_fdputstr(STDOUT_FILENO, (str))
#define ldso_putchar(c) ldso_fdputchar(STDOUT_FILENO, (c))

#endif
