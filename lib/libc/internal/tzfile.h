/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * tzfile.h - reader for TZif (RFC 9636) zone files, version 2 and later.
 *
 * A loaded zone is one immutable allocation. The caller keeps it for the life
 * of the process once it has published it (and frees it otherwise), so pointers
 * into a kept zone (zone abbreviations, the footer) stay valid. Only the 64-bit
 * block of the file is used. A file whose 64-bit block has leap second records
 * is rejected, not loaded: its transition times are not POSIX time. The
 * standard/wall and UT/local indicators are skipped: their counts must be 0 or
 * the number of types, and their values are not read.
 */
#ifndef LIBC_TZFILE_H
#define LIBC_TZFILE_H

#include <stddef.h>
#include <sys/cdefs.h>

/* Where zone files live, with a trailing slash. */
#ifndef TZ_DIR_PREFIX
#define TZ_DIR_PREFIX "/usr/share/zoneinfo/"
#endif

/* Longest zone file read, in bytes. The largest zone in tzdata is under 4 KiB
 * (fat) and a file is read once, so this bounds both time and memory.
 */
#define TZFILE_MAX 65536

/* Longest POSIX TZ footer kept, without its newlines. */
#define TZFILE_FOOTER_MAX 128

/* Largest UT offset RFC 9636 allows, in seconds. */
#define UTOFF_MAX 93600L

/* One local time type: seconds east of UTC, DST flag and abbreviation. */
struct tzfile_type {
    long gmtoff;
    int dst;
    const char *name;
};

struct tzfile;

/* Whether `name` is a plausible zone file name: relative, made of letters,
 * digits and "/_+-.", with no empty or dot-leading component.
 */
int __tzfile_name_ok(const char *name) __hidden;

/* Read and validate TZ_DIR_PREFIX + name, or return NULL. Allocates once. */
const struct tzfile *__tzfile_load(const char *name) __hidden;

/* The POSIX TZ footer of the file, "" if it has none. */
const char *__tzfile_footer(const struct tzfile *f) __hidden;

/* Whether UTC time t is after the last transition (or the file has none), where
 * the footer rule, if any, takes over.
 */
int __tzfile_after_table(const struct tzfile *f, long long t) __hidden;

/* The local time type in effect at UTC time t from the table alone: the type of
 * the latest transition at or before t, or type 0 before the first.
 */
void __tzfile_type_at(const struct tzfile *f,
                      long long t,
                      struct tzfile_type *out) __hidden;

/* Fill out (max entries) with the types of the transitions in (lo, hi], in time
 * order, and return how many there are (at most max).
 */
size_t __tzfile_types_in(const struct tzfile *f,
                         long long lo,
                         long long hi,
                         struct tzfile_type *out,
                         size_t max) __hidden;

/* The standard and DST types of the latest transitions of each kind. With no
 * standard transition, *std is type 0; with no DST transition, *dst is a copy of
 * *std (not type 0), and *has_dst, which says whether any DST type was seen, is
 * 0.
 */
void __tzfile_last_types(const struct tzfile *f,
                         struct tzfile_type *std,
                         struct tzfile_type *dst,
                         int *has_dst) __hidden;

#endif /* LIBC_TZFILE_H */
