/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* Semi-public interface from thread libraries to ldso for managing TLS. */

#ifndef _DYNLINK_TLS_H_
#define _DYNLINK_TLS_H_

/* Allocate a TLS block for a new thread. The memory allocated will include
 * 'tcbsize' bytes aligned to a 'tcbalign' boundary (in bytes) for the thread
 * library's private purposes. The location of the TCB block is returned by this
 * function. For architectures using 'Variant I' TLS, the thread local storage
 * follows the TCB, and for 'Variant II', the thread local storage precedes it.
 * For the 'Variant I' model used by RTMux, the TCB must begin with a single
 * pointer field for ldso's implementation.
 *
 * If the value of 'oldtls' is non-NULL, the new TLS block will be initialized
 * using the values contained in 'oldtls' and 'oldtls' will be freed. This is
 * typically used when initializing a thread library to migrate from using the
 * initial bootstrap TLS block created by ldso to one which contains suitable
 * thread library private data.
 *
 * The value returned from this function is suitable for installing directly
 * into the thread pointer register.
 */
void *_ldso_allocate_tls(void *oldtls,
                         size_t tcbsize,
                         size_t tcbalign) __exported;

/* Free a TLS block allocated using _ldso_allocate_tls(). The tcbsize and
 * tcbalign parameters must be the same as those used to allocate the block.
 */
void _ldso_free_tls(void *tcb, size_t tcbsize, size_t tcbalign) __exported;

#endif
