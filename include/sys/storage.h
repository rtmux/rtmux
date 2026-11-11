/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/storage.h */

#ifndef __STORAGE_H_INCLUDED
#define __STORAGE_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

__BEGIN_DECLS
/* Thread local storage. This data is at the top of each threads stack. */
struct _thread_local_storage {
    void (*__exitfunc)(void *);
    void *__arg;
    int *__errptr;
    int __errval; /* Not used for main() thread. */
    unsigned __flags;
    int __pid;
    int __tid;
    unsigned __owner;
    void *__stackaddr;
    unsigned __reserved1;
    unsigned __numkeys;
    void **__keydata; /* Indexed by pthread_key_t */
    void *__cleanup;
    void *__fpuemu_data;
    void *__reserved2[2];
};

/* Routine to return pointer to thread local storage structure. */
extern struct _thread_local_storage *__tls(void) __attribute__((__const__));


/* Process local storage. */
struct _process_local_storage {
    void *(*__getgot)(void *__pltaddr);
    void (*__mathemulator)(unsigned __sigcode, void **__pdata, void *__regs);
    void *__dll_list;
    void *__reserved[6];
    void (*__threadwatch)(int __tid);
};

__END_DECLS
#endif
