/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* ftype.h File types. */
#ifndef __FTYPE_H_INCLUDED
#define __FTYPE_H_INCLUDED

enum _file_type {
    _FTYPE_MATCHED = -1,
    _FTYPE_ALL = -1,
    _FTYPE_ANY = 0,
    _FTYPE_FILE = 1,
    _FTYPE_LINK = 2,
    _FTYPE_PIPE = 4,
    _FTYPE_SHMEM = 5,
    _FTYPE_SOCKET = 7,
    _FTYPE_SEM = 8,
    _FTYPE_DUMPER = 10,
    _FTYPE_MOUNT = 11,
    _FTYPE_NAME = 12,
    _FTYPE_TYMEM = 13
};

#define _MAJOR_FSYS "fsys" /* All filesystems */

#endif
