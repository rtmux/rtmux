/* SPDX-License-Identifier: Apache-2.0 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/rtmux-core.h>
#include <sys/ioserv-private.h>
#include "api.h"
#include "internal.h"

static struct link registered = {.id = 7, .count = 1};

int *__get_errno_ptr(void)
{
    static int error;
    return &error;
}

struct link *_ioserv_link_query(int id, int countadj)
{
    assert(id == registered.id);
    registered.count += countadj;
    return &registered;
}

void _ioserv_link_return(struct link *link, int countadj)
{
    assert(link == &registered);
    link->count -= countadj;
}

void *_ioserv_handle(struct _msg_info *info,
                     void *handle,
                     enum _ioserv_handle_type type)
{
    (void) info;
    (void) handle;
    (void) type;
    abort();
}

void *fail_binding_alloc(size_t count, size_t size)
{
    assert(count * size == sizeof(struct binding));
    return NULL;
}

int main(void)
{
    ioserv_context_t ctx = {.id = registered.id};

    assert(ioserv_backend_open_bind(&ctx, NULL, NULL) == -1);
    assert(errno == ENOMEM);
    assert(registered.count == 1);
    return 0;
}
