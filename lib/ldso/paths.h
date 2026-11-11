/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RTMux dynamic-linker paths.
 *
 * Defines the search paths and naming conventions for the RTMux dynamic linker
 * (ld-rtmux.so).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _DYNLINK_PATHS_H
#define _DYNLINK_PATHS_H

/* The ldso binary itself */
#define _BASENAME_LDSO "ld-rtmux.so.1"
#define _PATH_LDSO "/proc/boot/ld-rtmux.so.1"

/* Standard library search path.
 *
 * /lib -- system libraries (symlink to /rd/lib during boot) /usr/lib --
 * additional libraries (symlink to /rd/lib during boot) /rd/lib -- CPIO ramdisk
 * libraries (direct path)
 */
#define STANDARD_LIBRARY_PATH "/lib:/usr/lib:/rd/lib"

/* LD_ environment variable prefix. RTMux uses plain "LD_". */
#define LD_ "LD_"

/* Token substitution: $LIB expands to this */
#define TOKEN_LIB "lib"

/* Variables exported when IN_LDSO is defined (for use by dynlink.c). */
#ifdef IN_LDSO
extern const char *ld_path_ldso;
extern const char *ld_standard_library_path;
extern const char *ld_env_prefix;
#endif

#endif /* _DYNLINK_PATHS_H */
