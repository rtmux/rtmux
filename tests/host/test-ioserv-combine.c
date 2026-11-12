/* SPDX-License-Identifier: Apache-2.0 */
/* Compile the real dispatcher with a service whose combined operation fails
 * after open succeeded. The failure must roll back the newly bound OCB. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../lib/libc/ioserv/connect-handler.c"

pthread_key_t _ioserv_thread_key;
static void *tls_link;
static struct binding binding;
static int bound, closes, operation_result;
void atomic_add(volatile unsigned *p, unsigned value) { *p += value; }

static int open_ok(ioserv_context_t *ctp, io_open_t *msg, void *h, void *extra)
{
    (void) ctp; (void) msg; (void) h; (void) extra;
    assert(!bound);
    bound = 1;
    return EOK;
}
static const ioserv_connect_ops_t ops = {.nfuncs = IOSERV_CONNECT_NFUNCS, .open = open_ok};
static struct link service = {.id = 3, .connect_funcs = &ops};

struct link *_ioserv_link_query(int id, int adj) { (void) id; (void) adj; return &service; }
void _ioserv_link_return(struct link *p, int adj) { assert(p == &service); (void) adj; }
void *pthread_getspecific(pthread_key_t key) { (void) key; return tls_link; }
int pthread_setspecific(pthread_key_t key, const void *p) { (void) key; tls_link = (void *) p; return 0; }
void *_ioserv_handle(struct _msg_info *info, void *p, enum _ioserv_handle_type type)
{
    (void) info; (void) p; (void) type;
    return bound ? &binding : (void *) -1;
}
void _ioserv_close_handler(ioserv_context_t *ctp, struct binding *b) { (void) ctp; assert(b == &binding); }
int _ioserv_io_handler(ioserv_context_t *ctp, ioserv_msg_t *msg, struct binding *b)
{
    (void) ctp;
    assert(b == &binding);
    if (msg->type == _IO_CLOSE) {
        assert(bound);
        bound = 0;
        closes++;
        return EIO; /* a cleanup error must not replace the operation's result */
    }
    assert(msg->type == _IO_STAT);
    return operation_result;
}

static void run(unsigned subtype, int result, int want_closed)
{
    union { uint64_t align; unsigned char bytes[1024]; } storage = {0};
    ioserv_context_t ctx = {0};
    ioserv_msg_t *msg = (void *) storage.bytes;
    struct _io_combine *extra = align_msg(msg, offsetof(struct _io_connect, path), 1);
    ctx.msg = msg;
    ctx.rcvid = 42;
    ctx.info.msglen = sizeof(storage);
    msg->open.connect.subtype = subtype;
    msg->open.connect.path_len = 1;
    msg->open.connect.extra_len = sizeof(*extra);
    extra->type = _IO_STAT;
    extra->combine_len = sizeof(*extra);
    bound = closes = 0;
    operation_result = result;
    assert(_ioserv_connect_handler(&ctx, msg) == result);
    assert(closes == want_closed);
    assert(bound == !want_closed);
    assert(ctx.rcvid == 42);
}

int main(void)
{
    run(_IO_CONNECT_COMBINE, ENOSYS, 1);
    run(_IO_CONNECT_COMBINE, EIO, 1);
    run(_IO_CONNECT_COMBINE, IOSERV_DEFAULT, 1);
    run(_IO_CONNECT_COMBINE, EOK, 0);
    run(_IO_CONNECT_COMBINE, IOSERV_NPARTS(1), 0);
    run(_IO_CONNECT_COMBINE, IOSERV_NOREPLY, 0);
    run(_IO_CONNECT_COMBINE_CLOSE, IOSERV_NPARTS(1), 1);
    run(_IO_CONNECT_COMBINE_CLOSE, ENOSYS, 1);
    puts("ioserv-combine-test: PASS (8 transaction outcomes)");
    return 0;
}
