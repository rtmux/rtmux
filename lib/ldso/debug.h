/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 *
 * Support for printing debugging messages.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef DEBUG_H
#define DEBUG_H 1

#include <sys/cdefs.h>

#include <string.h>
#include "paths.h"
#include "printf.h"

void debug_printf(const char *, ...) __printflike(1, 2) __hidden;
extern int debug __hidden;

#ifndef NO_LD_DEBUG
#define dbg(...) debug_printf(__VA_ARGS__)
#else
#define dbg(...) ((void) 0)
#endif

#define assert(cond)                                             \
    ((cond) ? (void) 0                                           \
            : (msg(_BASENAME_LDSO ": assert failed: " __FILE__   \
                                  ":" __XSTRING(__LINE__) "\n"), \
               abort()))
#define msg(s) ldso_putstr(s)
#define trace() msg(_BASENAME_LDSO ": " __XSTRING(__LINE__) "\n")


#endif /* DEBUG_H */
