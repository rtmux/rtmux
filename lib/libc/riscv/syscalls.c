/*
 * RTMux libc — RISC-V syscall stubs
 *
 * Every non-spare __KSYS_* call gets:
 *   Foo_r()  — raw ecall, returns kernel value directly
 *   Foo()    — wraps _r, sets errno on negative, returns -1
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/calls.h>

/* Pull in the RTMUX_FLG_CHF_* channel flags without the syscall prototypes:
 * this file defines those syscalls, so the protos would conflict.
 */
#define _RTMUX_NO_SYSCALL_PROTOS
#include <sys/rtmux-core.h>
#include <sys/rtmux-ident.h>
#include <sys/compiler.h>
#include <stddef.h>
#include <stdint.h>

/* These stubs only pass a "const struct sigevent *" through to the kernel as an
 * opaque pointer (never dereferenced here), so a forward declaration is all we
 * need — matching <unistd.h>. Without it the type would be scoped to the
 * parameter list and warned about.
 */
struct sigevent;

/* .rtmux section identification */
RTMUX_IDENT(RTMUX_TYPE_SHLIB, 0, 1);

/* RISC-V ecall wrappers (U-mode -> S-mode trap, scause=8)
 *
 *   a7      = syscall number (__KSYS_*)
 *   a0-a5   = arguments 1-6
 *   a0      = return value
 */

ALWAYS_INLINE long __sc0(long n)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0");
    __asm__ volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

ALWAYS_INLINE long __sc1(long n, long _a0)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = _a0;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

ALWAYS_INLINE long __sc2(long n, long _a0, long _a1)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = _a0;
    register long a1 __asm__("a1") = _a1;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

ALWAYS_INLINE long __sc3(long n, long _a0, long _a1, long _a2)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = _a0;
    register long a1 __asm__("a1") = _a1;
    register long a2 __asm__("a2") = _a2;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

ALWAYS_INLINE long __sc4(long n, long _a0, long _a1, long _a2, long _a3)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = _a0;
    register long a1 __asm__("a1") = _a1;
    register long a2 __asm__("a2") = _a2;
    register long a3 __asm__("a3") = _a3;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a7)
                     : "memory");
    return a0;
}

ALWAYS_INLINE long __sc5(long n,
                         long _a0,
                         long _a1,
                         long _a2,
                         long _a3,
                         long _a4)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = _a0;
    register long a1 __asm__("a1") = _a1;
    register long a2 __asm__("a2") = _a2;
    register long a3 __asm__("a3") = _a3;
    register long a4 __asm__("a4") = _a4;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
                     : "memory");
    return a0;
}

ALWAYS_INLINE long
__sc6(long n, long _a0, long _a1, long _a2, long _a3, long _a4, long _a5)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = _a0;
    register long a1 __asm__("a1") = _a1;
    register long a2 __asm__("a2") = _a2;
    register long a3 __asm__("a3") = _a3;
    register long a4 __asm__("a4") = _a4;
    register long a5 __asm__("a5") = _a5;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                     : "memory");
    return a0;
}

/* Errno helper */
static inline int __sc_err(long ret)
{
    if (ret < 0) {
        errno = (int) (-ret);
        return -1;
    }
    return (int) ret;
}

/* Shorthand: L casts to long */
#define L(x) ((long) (x))

/* All syscall functions are weak so kernel-provided fast paths can override
 * them where needed. User-space programs get these ecall-based versions by
 * default.
 */
#define WEAK __attribute__((weak))
#define WEAK_HIDDEN __attribute__((weak, visibility("hidden")))

/* System */

/* 0: NOP */
WEAK_HIDDEN int Nop_r(void)
{
    return (int) __sc0(__KSYS_NOP);
}

/* 1: trace event */
WEAK_HIDDEN int trace_event_r(unsigned *data)
{
    return (int) __sc1(__KSYS_TRACE_EVENT, L(data));
}

/* Public entry point for the trace control syscall (tracelogger, user events).
 * The _r form above is hidden for libc-internal instrumentation; this exported
 * variant reports errors through errno like the other public wrappers.
 */
WEAK int trace_event(unsigned *data)
{
    return __sc_err(__sc1(__KSYS_TRACE_EVENT, L(data)));
}

/* 2: roottask-privileged kcall by sub-ID.
 *
 *   a7 = __KSYS_PRIV
 *   a0 = id     (one of __KSYS_PRIV_*, see <private/priv-calls.h>)
 *   a1 = arg    (pointer passed to the in-kernel handler)
 *
 * The kernel-side dispatcher gates on per-operation priv_caps; callers without
 * the matching cap get -EPERM, out-of-range ids get -ENOSYS. No function
 * pointer crosses the kernel↔userspace boundary.
 */
WEAK long syscall_priv(int id, void *arg)
{
    return __sc2(__KSYS_PRIV, L(id), L(arg));
}

/* 7: SysCpupageGet */
WEAK_HIDDEN long SysCpupageGet_r(int index)
{
    return __sc1(__KSYS_SYS_CPUPAGE_GET, L(index));
}

/* 8: SysCpupageSet */
WEAK_HIDDEN int SysCpupageSet_r(int index, long value)
{
    return (int) __sc2(__KSYS_SYS_CPUPAGE_SET, L(index), value);
}

/* 3: dbgprint — print a string via the kernel console */
WEAK void dbgprint(const char *str, size_t len)
{
    __sc2(__KSYS_DBGPRINT, L(str), L(len));
}

/* Messages */

/* 10: msg_current */
WEAK int msg_current_r(int rcvid)
{
    return (int) __sc1(__KSYS_MSG_CURRENT, L(rcvid));
}

WEAK int msg_current(int rcvid)
{
    return __sc_err(__sc1(__KSYS_MSG_CURRENT, L(rcvid)));
}

/* 11: msg_sendv */
WEAK int msg_sendv_r(int coid,
                     const void *siov,
                     int sparts,
                     const void *riov,
                     int rparts)
{
    return (int) __sc5(__KSYS_MSG_SENDV, L(coid), L(siov), L(sparts), L(riov),
                       L(rparts));
}

WEAK int msg_sendv(int coid,
                   const void *siov,
                   int sparts,
                   const void *riov,
                   int rparts)
{
    return __sc_err(__sc5(__KSYS_MSG_SENDV, L(coid), L(siov), L(sparts),
                          L(riov), L(rparts)));
}

WEAK int msg_sendv_cap_r(uint64_t cap,
                         const void *siov,
                         int sparts,
                         const void *riov,
                         int rparts)
{
    return (int) __sc5(__KSYS_MSG_SENDV, L(cap), L(siov), L(sparts), L(riov),
                       L(rparts));
}

WEAK int msg_sendv_cap(uint64_t cap,
                       const void *siov,
                       int sparts,
                       const void *riov,
                       int rparts)
{
    return __sc_err(__sc5(__KSYS_MSG_SENDV, L(cap), L(siov), L(sparts), L(riov),
                          L(rparts)));
}

/* 12: msg_sendvnc */
WEAK int msg_sendvnc_r(int coid,
                       const void *siov,
                       int sparts,
                       const void *riov,
                       int rparts)
{
    return (int) __sc5(__KSYS_MSG_SENDVNC, L(coid), L(siov), L(sparts), L(riov),
                       L(rparts));
}

WEAK int msg_sendvnc(int coid,
                     const void *siov,
                     int sparts,
                     const void *riov,
                     int rparts)
{
    return __sc_err(__sc5(__KSYS_MSG_SENDVNC, L(coid), L(siov), L(sparts),
                          L(riov), L(rparts)));
}

/* Flat-buffer convenience wrappers. RTMux syscall ABI convention: a negative
 * "parts" count means the pointer is a direct buffer of |parts| bytes, not an
 * iov_t array.
 */

/* 11b: msg_send — flat-buffer variant of msg_sendv */
WEAK int msg_send_r(int coid,
                    const void *smsg,
                    size_t sbytes,
                    void *rmsg,
                    size_t rbytes)
{
    return (int) __sc5(__KSYS_MSG_SENDV, L(coid), L(smsg), L(-(long) sbytes),
                       L(rmsg), L(-(long) rbytes));
}

WEAK int msg_send(int coid,
                  const void *smsg,
                  size_t sbytes,
                  void *rmsg,
                  size_t rbytes)
{
    return __sc_err(__sc5(__KSYS_MSG_SENDV, L(coid), L(smsg), L(-(long) sbytes),
                          L(rmsg), L(-(long) rbytes)));
}

WEAK int msg_send_cap_r(uint64_t cap,
                        const void *smsg,
                        size_t sbytes,
                        void *rmsg,
                        size_t rbytes)
{
    return (int) __sc5(__KSYS_MSG_SENDV, L(cap), L(smsg), L(-(long) sbytes),
                       L(rmsg), L(-(long) rbytes));
}

WEAK int msg_send_cap(uint64_t cap,
                      const void *smsg,
                      size_t sbytes,
                      void *rmsg,
                      size_t rbytes)
{
    return __sc_err(__sc5(__KSYS_MSG_SENDV, L(cap), L(smsg), L(-(long) sbytes),
                          L(rmsg), L(-(long) rbytes)));
}

/* 12b: msg_sendnc — flat-buffer variant of msg_sendvnc (non-cancellable) */
WEAK int msg_sendnc_r(int coid,
                      const void *smsg,
                      size_t sbytes,
                      void *rmsg,
                      size_t rbytes)
{
    return (int) __sc5(__KSYS_MSG_SENDVNC, L(coid), L(smsg), L(-(long) sbytes),
                       L(rmsg), L(-(long) rbytes));
}

WEAK int msg_sendnc(int coid,
                    const void *smsg,
                    size_t sbytes,
                    void *rmsg,
                    size_t rbytes)
{
    return __sc_err(__sc5(__KSYS_MSG_SENDVNC, L(coid), L(smsg),
                          L(-(long) sbytes), L(rmsg), L(-(long) rbytes)));
}

/* 15b: msg_reply — flat-buffer variant of msg_replyv */
WEAK int msg_reply_r(int rcvid, int status, const void *msg, int bytes)
{
    return (int) __sc4(__KSYS_MSG_REPLYV, L(rcvid), L(status), L(msg),
                       L(-bytes));
}

WEAK int msg_reply(int rcvid, int status, const void *msg, int bytes)
{
    return __sc_err(
        __sc4(__KSYS_MSG_REPLYV, L(rcvid), L(status), L(msg), L(-bytes)));
}

/* 16b: msg_read — flat-buffer variant of msg_readv */
WEAK int msg_read_r(int rcvid, void *msg, int bytes, int offset)
{
    return (int) __sc4(__KSYS_MSG_READV, L(rcvid), L(msg), L(-bytes),
                       L(offset));
}

WEAK int msg_read(int rcvid, void *msg, int bytes, int offset)
{
    return __sc_err(
        __sc4(__KSYS_MSG_READV, L(rcvid), L(msg), L(-bytes), L(offset)));
}

/* 17b: msg_write — flat-buffer variant of msg_writev */
WEAK int msg_write_r(int rcvid, const void *msg, int bytes, int offset)
{
    return (int) __sc4(__KSYS_MSG_WRITEV, L(rcvid), L(msg), L(-bytes),
                       L(offset));
}

WEAK int msg_write(int rcvid, const void *msg, int bytes, int offset)
{
    return __sc_err(
        __sc4(__KSYS_MSG_WRITEV, L(rcvid), L(msg), L(-bytes), L(offset)));
}

/* 13: msg_error */
WEAK int msg_error_r(int rcvid, int err)
{
    return (int) __sc2(__KSYS_MSG_ERROR, L(rcvid), L(err));
}

WEAK int msg_error(int rcvid, int err)
{
    return __sc_err(__sc2(__KSYS_MSG_ERROR, L(rcvid), L(err)));
}

/* 14: msg_receivev */
WEAK int msg_receivev_r(int chid, const void *iov, int parts, void *info)
{
    return (int) __sc4(__KSYS_MSG_RECEIVEV, L(chid), L(iov), L(parts), L(info));
}

WEAK int msg_receivev(int chid, const void *iov, int parts, void *info)
{
    return __sc_err(
        __sc4(__KSYS_MSG_RECEIVEV, L(chid), L(iov), L(parts), L(info)));
}

/* 14b: msg_receive — flat-buffer variant of msg_receivev */
WEAK int msg_receive_r(int chid, void *msg, size_t bytes, void *info)
{
    return (int) __sc4(__KSYS_MSG_RECEIVEV, L(chid), L(msg), L(-(long) bytes),
                       L(info));
}

WEAK int msg_receive(int chid, void *msg, size_t bytes, void *info)
{
    return __sc_err(
        __sc4(__KSYS_MSG_RECEIVEV, L(chid), L(msg), L(-(long) bytes), L(info)));
}

/* 15: msg_replyv */
WEAK int msg_replyv_r(int rcvid, int status, const void *iov, int parts)
{
    return (int) __sc4(__KSYS_MSG_REPLYV, L(rcvid), L(status), L(iov),
                       L(parts));
}

WEAK int msg_replyv(int rcvid, int status, const void *iov, int parts)
{
    return __sc_err(
        __sc4(__KSYS_MSG_REPLYV, L(rcvid), L(status), L(iov), L(parts)));
}

/* 16: msg_readv */
WEAK int msg_readv_r(int rcvid, const void *iov, int parts, int offset)
{
    return (int) __sc4(__KSYS_MSG_READV, L(rcvid), L(iov), L(parts), L(offset));
}

WEAK int msg_readv(int rcvid, const void *iov, int parts, int offset)
{
    return __sc_err(
        __sc4(__KSYS_MSG_READV, L(rcvid), L(iov), L(parts), L(offset)));
}

/* 17: msg_writev */
WEAK int msg_writev_r(int rcvid, const void *iov, int parts, int offset)
{
    return (int) __sc4(__KSYS_MSG_WRITEV, L(rcvid), L(iov), L(parts),
                       L(offset));
}

WEAK int msg_writev(int rcvid, const void *iov, int parts, int offset)
{
    return __sc_err(
        __sc4(__KSYS_MSG_WRITEV, L(rcvid), L(iov), L(parts), L(offset)));
}

/* 18: msg_read_writev removed — kernel slot bound to ksys_bad. */

/* 19: msg_info */
WEAK int msg_info_r(int rcvid, void *info)
{
    return (int) __sc2(__KSYS_MSG_INFO, L(rcvid), L(info));
}

WEAK int msg_info(int rcvid, void *info)
{
    return __sc_err(__sc2(__KSYS_MSG_INFO, L(rcvid), L(info)));
}

/* 20: msg_send_notification */
WEAK int msg_send_notification_r(int coid, int priority, int code, int value)
{
    return (int) __sc4(__KSYS_MSG_SEND_NOTIFICATION, L(coid), L(priority),
                       L(code), L(value));
}

WEAK int msg_send_notification(int coid, int priority, int code, int value)
{
    return __sc_err(__sc4(__KSYS_MSG_SEND_NOTIFICATION, L(coid), L(priority),
                          L(code), L(value)));
}

WEAK int msg_send_notification_cap_r(uint64_t cap,
                                     int priority,
                                     int code,
                                     int value)
{
    return (int) __sc4(__KSYS_MSG_SEND_NOTIFICATION, L(cap), L(priority),
                       L(code), L(value));
}

WEAK int msg_send_notification_cap(uint64_t cap,
                                   int priority,
                                   int code,
                                   int value)
{
    return __sc_err(__sc4(__KSYS_MSG_SEND_NOTIFICATION, L(cap), L(priority),
                          L(code), L(value)));
}

/* 21: msg_deliver_event */
WEAK int msg_deliver_event_r(int rcvid, const void *event)
{
    return (int) __sc2(__KSYS_MSG_DELIVER_EVENT, L(rcvid), L(event));
}

WEAK int msg_deliver_event(int rcvid, const void *event)
{
    return __sc_err(__sc2(__KSYS_MSG_DELIVER_EVENT, L(rcvid), L(event)));
}

/* 22: msg_key_data */
WEAK int msg_key_data_r(int rcvid,
                        int op,
                        unsigned key,
                        unsigned *newkey,
                        const void *iov,
                        int parts)
{
    return (int) __sc6(__KSYS_MSG_KEYDATA, L(rcvid), L(op), L(key), L(newkey),
                       L(iov), L(parts));
}

WEAK int msg_key_data(int rcvid,
                      int op,
                      unsigned key,
                      unsigned *newkey,
                      const void *iov,
                      int parts)
{
    return __sc_err(__sc6(__KSYS_MSG_KEYDATA, L(rcvid), L(op), L(key),
                          L(newkey), L(iov), L(parts)));
}

/* 23: msg_readiov */
WEAK int msg_readiov_r(int rcvid,
                       const void *iov,
                       int parts,
                       int offset,
                       int flags)
{
    return (int) __sc5(__KSYS_MSG_READIOV, L(rcvid), L(iov), L(parts),
                       L(offset), L(flags));
}

WEAK int msg_readiov(int rcvid,
                     const void *iov,
                     int parts,
                     int offset,
                     int flags)
{
    return __sc_err(__sc5(__KSYS_MSG_READIOV, L(rcvid), L(iov), L(parts),
                          L(offset), L(flags)));
}

/* 24: msg_receive_notificationv */
WEAK int msg_receive_notificationv_r(int chid,
                                     const void *iov,
                                     int parts,
                                     void *info)
{
    return (int) __sc4(__KSYS_MSG_RECEIVENOTIFICATIONV, L(chid), L(iov),
                       L(parts), L(info));
}

WEAK int msg_receive_notificationv(int chid,
                                   const void *iov,
                                   int parts,
                                   void *info)
{
    return __sc_err(__sc4(__KSYS_MSG_RECEIVENOTIFICATIONV, L(chid), L(iov),
                          L(parts), L(info)));
}

/* 24b: msg_receive_notification — flat-buffer variant of
 * msg_receive_notificationv
 */
WEAK int msg_receive_notification_r(int chid,
                                    void *notification,
                                    size_t bytes,
                                    void *info)
{
    /* bytes is encoded as a negative rparts; a value > INT32_MAX would wrap and
     * be misread by the kernel as a positive iov-vector count.
     */
    if (bytes > INT32_MAX)
        return -EINVAL;
    return (int) __sc4(__KSYS_MSG_RECEIVENOTIFICATIONV, L(chid),
                       L(notification), L(-(long) bytes), L(info));
}

WEAK int msg_receive_notification(int chid,
                                  void *notification,
                                  size_t bytes,
                                  void *info)
{
    if (bytes > INT32_MAX)
        return __sc_err(-EINVAL);
    return __sc_err(__sc4(__KSYS_MSG_RECEIVENOTIFICATIONV, L(chid),
                          L(notification), L(-(long) bytes), L(info)));
}

/* 25: msg_verify_event */
WEAK int msg_verify_event_r(int rcvid, const void *event)
{
    return (int) __sc2(__KSYS_MSG_VERIFY_EVENT, L(rcvid), L(event));
}

WEAK int msg_verify_event(int rcvid, const void *event)
{
    return __sc_err(__sc2(__KSYS_MSG_VERIFY_EVENT, L(rcvid), L(event)));
}

/* Channels & Connections */

/* 35: channel_create.
 *
 * Zero the extended arg slots (bufsize/maxbuf/event/cred/cap_out): the kernel
 * copies all six arg registers, and the global-channel path dereferences the
 * event/cred pointers. Passing leftover register garbage there would fault, so
 * a plain create must present them as NULL.
 */
WEAK int channel_create_r(unsigned flags)
{
    return (int) __sc6(__KSYS_CHANNEL_CREATE, L(flags), 0, 0, 0, 0, 0);
}

WEAK int channel_create(unsigned flags)
{
    return __sc_err(__sc6(__KSYS_CHANNEL_CREATE, L(flags), 0, 0, 0, 0, 0));
}

/* Create a capability-gated global channel. cap_out receives the minted gating
 * cap id in arg slot 1 (a1); the kernel reads it only under
 * RTMUX_FLG_CHF_CAP_GATED, which this wrapper forces on. The bufsize/maxbuf/
 * event/cred slots are 0.
 */
WEAK int channel_create_cap(unsigned flags, uint64_t *cap_out)
{
    return __sc_err(
        __sc6(__KSYS_CHANNEL_CREATE,
              L(flags | RTMUX_FLG_CHF_CAP_GATED | RTMUX_FLG_CHF_GLOBAL),
              L(cap_out), 0, 0, 0, 0));
}

/* 36: channel_destroy */
WEAK int channel_destroy_r(int chid)
{
    return (int) __sc1(__KSYS_CHANNEL_DESTROY, L(chid));
}

WEAK int channel_destroy(int chid)
{
    return __sc_err(__sc1(__KSYS_CHANNEL_DESTROY, L(chid)));
}

/* 39: connect_attach */
WEAK int connect_attach_r(unsigned nd,
                          int pid,
                          int chid,
                          unsigned index,
                          int flags)
{
    return (int) __sc5(__KSYS_CONNECT_ATTACH, L(nd), L(pid), L(chid), L(index),
                       L(flags));
}

WEAK int connect_attach(unsigned nd,
                        int pid,
                        int chid,
                        unsigned index,
                        int flags)
{
    return __sc_err(__sc5(__KSYS_CONNECT_ATTACH, L(nd), L(pid), L(chid),
                          L(index), L(flags)));
}

WEAK int connect_attach_cap_r(unsigned nd,
                              int pid,
                              int chid,
                              unsigned index,
                              int flags,
                              uint64_t *cap)
{
    struct connect_attach_cap_info {
        uint32_t nd;
        uint32_t srcnd;
        int32_t pid;
        int32_t tid;
        int32_t chid;
        int32_t scoid;
        int32_t coid;
        uint32_t msglen;
        uint32_t srcmsglen;
        uint32_t dstmsglen;
        int16_t priority;
        uint16_t flags;
        uint32_t endpoint_rights;
        uint64_t endpoint_cap;
    } info;
    int coid;
    int saved_errno;

    if (!cap)
        return EINVAL;
    *cap = 0;

    /* The cap aliases the connection created by connect_attach; the numeric
     * coid is its backing reference (detaching the coid revokes the cap), so it
     * stays in fdcons and is reclaimed at process exit or by detaching via the
     * cap. A coid-free, independently-lifetimed cap would need a kernel
     * cap-only attach path — add that if the fd slot matters.
     */
    coid = connect_attach(nd, pid, chid, index, flags);
    if (coid == -1)
        return errno;
    if (__sc_err(__sc3(__KSYS_CONNECT_SERVER_INFO, L(0), L((uint64_t) coid),
                       L(&info))) == -1) {
        saved_errno = errno;
        (void) __sc_err(__sc1(__KSYS_CONNECT_DETACH, L((uint64_t) coid)));
        errno = saved_errno;
        return saved_errno;
    }
    *cap = info.endpoint_cap;
    return EOK;
}

WEAK uint64_t
connect_attach_cap(unsigned nd, int pid, int chid, unsigned index, int flags)
{
    uint64_t cap = 0;
    int r = connect_attach_cap_r(nd, pid, chid, index, flags, &cap);

    if (r != EOK) {
        errno = r;
        return 0;
    }
    return cap;
}

/* 40: __connect_detach */
WEAK int connect_detach_r(uint64_t coid)
{
    return (int) __sc1(__KSYS_CONNECT_DETACH, L(coid));
}

WEAK int __connect_detach(uint64_t coid)
{
    return __sc_err(__sc1(__KSYS_CONNECT_DETACH, L(coid)));
}

/* 41: connect_server_info */
WEAK int connect_server_info_r(int pid, uint64_t coid, void *info)
{
    return (int) __sc3(__KSYS_CONNECT_SERVER_INFO, L(pid), L(coid), L(info));
}

WEAK int connect_server_info(int pid, uint64_t coid, void *info)
{
    return __sc_err(
        __sc3(__KSYS_CONNECT_SERVER_INFO, L(pid), L(coid), L(info)));
}

/* 42: connect_client_info */
WEAK int connect_client_info_r(int scoid, void *info, int ngroups)
{
    return (int) __sc3(__KSYS_CONNECT_CLIENT_INFO, L(scoid), L(info),
                       L(ngroups));
}

WEAK int connect_client_info(int scoid, void *info, int ngroups)
{
    return __sc_err(
        __sc3(__KSYS_CONNECT_CLIENT_INFO, L(scoid), L(info), L(ngroups)));
}

/* 43: connect_flags */
WEAK int connect_flags_r(int pid, uint64_t coid, unsigned mask, unsigned bits)
{
    return (int) __sc4(__KSYS_CONNECT_FLAGS, L(pid), L(coid), L(mask), L(bits));
}

WEAK int connect_flags(int pid, uint64_t coid, unsigned mask, unsigned bits)
{
    return __sc_err(
        __sc4(__KSYS_CONNECT_FLAGS, L(pid), L(coid), L(mask), L(bits)));
}

/* Threads */

/* 46: __thread_create */
WEAK int thread_create_r(int pid, void *func, void *arg, void *attr)
{
    return (int) __sc4(__KSYS_THREAD_CREATE, L(pid), L(func), L(arg), L(attr));
}

WEAK int __thread_create(int pid, void *func, void *arg, void *attr)
{
    return __sc_err(
        __sc4(__KSYS_THREAD_CREATE, L(pid), L(func), L(arg), L(attr)));
}

/* 47: __thread_destroy */
WEAK int thread_destroy_r(int tid, int priority, void *status)
{
    return (int) __sc3(__KSYS_THREAD_DESTROY, L(tid), L(priority), L(status));
}

WEAK int __thread_destroy(int tid, int priority, void *status)
{
    return __sc_err(
        __sc3(__KSYS_THREAD_DESTROY, L(tid), L(priority), L(status)));
}

/* 48: thread_destroy_all (internal) */
WEAK int thread_destroy_all_r(void)
{
    return (int) __sc0(__KSYS_THREAD_DESTROYALL);
}

/* 49: thread_detach */
WEAK int thread_detach_r(int tid)
{
    return (int) __sc1(__KSYS_THREAD_DETACH, L(tid));
}

WEAK int thread_detach(int tid)
{
    return __sc_err(__sc1(__KSYS_THREAD_DETACH, L(tid)));
}

/* 50: thread_join */
WEAK int thread_join_r(int tid, void **status)
{
    return (int) __sc2(__KSYS_THREAD_JOIN, L(tid), L(status));
}

WEAK int thread_join(int tid, void **status)
{
    return __sc_err(__sc2(__KSYS_THREAD_JOIN, L(tid), L(status)));
}

/* 51: __thread_cancel */
WEAK int thread_cancel_r(int tid, void (*canstub)(void))
{
    return (int) __sc2(__KSYS_THREAD_CANCEL, L(tid), L(canstub));
}

WEAK int __thread_cancel(int tid, void (*canstub)(void))
{
    return __sc_err(__sc2(__KSYS_THREAD_CANCEL, L(tid), L(canstub)));
}

/* 52: thread_ctl */
WEAK int thread_ctl_r(int cmd, void *data)
{
    return (int) __sc2(__KSYS_THREAD_CTL, L(cmd), L(data));
}

WEAK int thread_ctl(int cmd, void *data)
{
    return __sc_err(__sc2(__KSYS_THREAD_CTL, L(cmd), L(data)));
}

/* Interrupts */

/* 55: interrupt_attach_thread */
WEAK int interrupt_attach_thread_r(int intr, unsigned flags)
{
    return (int) __sc2(__KSYS_INTERRUPT_ATTACH_THREAD, L(intr), L(flags));
}

WEAK int interrupt_attach_thread(int intr, unsigned flags)
{
    return __sc_err(__sc2(__KSYS_INTERRUPT_ATTACH_THREAD, L(intr), L(flags)));
}

/* 56: __interrupt_attach_event (direct event delivery) */
WEAK int interrupt_attach_event_r(int intr,
                                  const struct sigevent *event,
                                  unsigned flags)
{
    return (int) __sc3(__KSYS_INTERRUPT_ATTACH_EVENT, L(intr), L(event),
                       L(flags));
}

WEAK int __interrupt_attach_event(int intr,
                                  const struct sigevent *event,
                                  unsigned flags)
{
    return __sc_err(
        __sc3(__KSYS_INTERRUPT_ATTACH_EVENT, L(intr), L(event), L(flags)));
}

/* 57: interrupt_detach */
WEAK int interrupt_detach_r(int id)
{
    return (int) __sc1(__KSYS_INTERRUPT_DETACH, L(id));
}

WEAK int interrupt_detach(int id)
{
    return __sc_err(__sc1(__KSYS_INTERRUPT_DETACH, L(id)));
}

/* 58: interrupt_wait */
WEAK int interrupt_wait_r(int flags, const void *timeout)
{
    return (int) __sc2(__KSYS_INTERRUPT_WAIT, L(flags), L(timeout));
}

WEAK int interrupt_wait(int flags, const void *timeout)
{
    return __sc_err(__sc2(__KSYS_INTERRUPT_WAIT, L(flags), L(timeout)));
}

/* 59: __interrupt_mask */
WEAK int interrupt_mask_r(int intr, int id)
{
    return (int) __sc2(__KSYS_INTERRUPT_MASK, L(intr), L(id));
}

WEAK int __interrupt_mask(int intr, int id)
{
    return __sc_err(__sc2(__KSYS_INTERRUPT_MASK, L(intr), L(id)));
}

/* 60: __interrupt_unmask */
WEAK int interrupt_unmask_r(int intr, int id)
{
    return (int) __sc2(__KSYS_INTERRUPT_UNMASK, L(intr), L(id));
}

WEAK int __interrupt_unmask(int intr, int id)
{
    return __sc_err(__sc2(__KSYS_INTERRUPT_UNMASK, L(intr), L(id)));
}

/* Clocks & Timers */

/* 65: __clock_time */
WEAK int __clock_time_r(int id, const void *newtime, void *oldtime)
{
    return (int) __sc3(__KSYS_CLOCK_TIME, L(id), L(newtime), L(oldtime));
}

WEAK int __clock_time(int id, const void *newtime, void *oldtime)
{
    return __sc_err(__sc3(__KSYS_CLOCK_TIME, L(id), L(newtime), L(oldtime)));
}

/* 67: __clock_period */
WEAK int __clock_period(int id, const void *newp, void *oldp, int reserved)
{
    return __sc_err(
        __sc4(__KSYS_CLOCK_PERIOD, L(id), L(newp), L(oldp), L(reserved)));
}

/* 68: __clock_id */
WEAK int __clock_id(int pid, int tid)
{
    return __sc_err(__sc2(__KSYS_CLOCK_ID, L(pid), L(tid)));
}

/* 70: __timer_create */
WEAK int __timer_create(int clockid, const void *event)
{
    return __sc_err(__sc2(__KSYS_TIMER_CREATE, L(clockid), L(event)));
}

/* 71: __timer_destroy */
WEAK int __timer_destroy(int id)
{
    return __sc_err(__sc1(__KSYS_TIMER_DESTROY, L(id)));
}

/* 72: __timer_settime */
WEAK int __timer_settime(int id, int flags, const void *itime, void *oitime)
{
    return __sc_err(
        __sc4(__KSYS_TIMER_SETTIME, L(id), L(flags), L(itime), L(oitime)));
}

/* 73: __timer_info */
WEAK int __timer_info(int pid, int id, int flags, void *info)
{
    return __sc_err(__sc4(__KSYS_TIMER_INFO, L(pid), L(id), L(flags), L(info)));
}

/* 75: __timer_timeout */
WEAK int __timer_timeout_r(int clockid,
                           int flags,
                           const void *event,
                           const void *ntime,
                           void *otime)
{
    return (int) __sc5(__KSYS_TIMER_TIMEOUT, L(clockid), L(flags), L(event),
                       L(ntime), L(otime));
}

WEAK int __timer_timeout(int clockid,
                         int flags,
                         const void *event,
                         const void *ntime,
                         void *otime)
{
    return __sc_err(__sc5(__KSYS_TIMER_TIMEOUT, L(clockid), L(flags), L(event),
                          L(ntime), L(otime)));
}

/* Synchronization */

/* 78: sync_type_create */
WEAK int sync_type_create_r(unsigned type, void *sync, const void *attr)
{
    return (int) __sc3(__KSYS_SYNC_CREATE, L(type), L(sync), L(attr));
}

WEAK int sync_type_create(unsigned type, void *sync, const void *attr)
{
    return __sc_err(__sc3(__KSYS_SYNC_CREATE, L(type), L(sync), L(attr)));
}

/* 79: __sync_destroy */
WEAK int sync_destroy_r(void *sync)
{
    return (int) __sc1(__KSYS_SYNC_DESTROY, L(sync));
}

WEAK int __sync_destroy(void *sync)
{
    return __sc_err(__sc1(__KSYS_SYNC_DESTROY, L(sync)));
}

/* 80: __sync_mutex_lock */
WEAK int sync_mutex_lock_r(void *sync)
{
    return (int) __sc1(__KSYS_SYNC_MUTEX_LOCK, L(sync));
}

WEAK int __sync_mutex_lock(void *sync)
{
    return __sc_err(__sc1(__KSYS_SYNC_MUTEX_LOCK, L(sync)));
}

/* 81: __sync_mutex_unlock */
WEAK int sync_mutex_unlock_r(void *sync)
{
    return (int) __sc1(__KSYS_SYNC_MUTEX_UNLOCK, L(sync));
}

WEAK int __sync_mutex_unlock(void *sync)
{
    return __sc_err(__sc1(__KSYS_SYNC_MUTEX_UNLOCK, L(sync)));
}

/* 82: sync_condvar_wait */
WEAK int sync_condvar_wait_r(void *sync, void *mutex)
{
    return (int) __sc2(__KSYS_SYNC_CONDVAR_WAIT, L(sync), L(mutex));
}

WEAK int sync_condvar_wait(void *sync, void *mutex)
{
    return __sc_err(__sc2(__KSYS_SYNC_CONDVAR_WAIT, L(sync), L(mutex)));
}

/* 83: sync_condvar_signal */
WEAK int sync_condvar_signal_r(void *sync, int all)
{
    return (int) __sc2(__KSYS_SYNC_CONDVAR_SIGNAL, L(sync), L(all));
}

WEAK int sync_condvar_signal(void *sync, int all)
{
    return __sc_err(__sc2(__KSYS_SYNC_CONDVAR_SIGNAL, L(sync), L(all)));
}

/* 84: sync_sem_post */
WEAK int sync_sem_post_r(void *sync)
{
    return (int) __sc1(__KSYS_SYNC_SEM_POST, L(sync));
}

WEAK int sync_sem_post(void *sync)
{
    return __sc_err(__sc1(__KSYS_SYNC_SEM_POST, L(sync)));
}

/* 85: sync_sem_wait */
WEAK int sync_sem_wait_r(void *sync, int tryto)
{
    return (int) __sc2(__KSYS_SYNC_SEM_WAIT, L(sync), L(tryto));
}

WEAK int sync_sem_wait(void *sync, int tryto)
{
    return __sc_err(__sc2(__KSYS_SYNC_SEM_WAIT, L(sync), L(tryto)));
}

/* 86: sync_ctl */
WEAK int sync_ctl_r(int cmd, void *sync, void *data)
{
    return (int) __sc3(__KSYS_SYNC_CTL, L(cmd), L(sync), L(data));
}

WEAK int sync_ctl(int cmd, void *sync, void *data)
{
    return __sc_err(__sc3(__KSYS_SYNC_CTL, L(cmd), L(sync), L(data)));
}

/* 87: __sync_mutex_revive */
WEAK int sync_mutex_revive_r(void *sync)
{
    return (int) __sc1(__KSYS_SYNC_MUTEX_REVIVE, L(sync));
}

WEAK int __sync_mutex_revive(void *sync)
{
    return __sc_err(__sc1(__KSYS_SYNC_MUTEX_REVIVE, L(sync)));
}

/* Scheduling */

/* 88: __sched_get */
WEAK int __sched_get_r(int pid, int tid, void *param)
{
    return (int) __sc3(__KSYS_SCHED_GET, L(pid), L(tid), L(param));
}

WEAK int __sched_get(int pid, int tid, void *param)
{
    return __sc_err(__sc3(__KSYS_SCHED_GET, L(pid), L(tid), L(param)));
}

/* 89: __sched_set */
WEAK int __sched_set(int pid, int tid, int algorithm, const void *param)
{
    return __sc_err(
        __sc4(__KSYS_SCHED_SET, L(pid), L(tid), L(algorithm), L(param)));
}

/* 90: __sched_yield */
WEAK int __sched_yield(void)
{
    return __sc_err(__sc0(__KSYS_SCHED_YIELD));
}

/* 91: __sched_info */
WEAK int __sched_info(int pid, int algorithm, void *info)
{
    return __sc_err(__sc3(__KSYS_SCHED_INFO, L(pid), L(algorithm), L(info)));
}

/* Misc */

/* MtCtl (syscall 100) removed */
