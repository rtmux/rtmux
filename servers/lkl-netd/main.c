/*
 * lkl-netd - gated LKL network personality prototype.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <align.h>

struct netd_ocb;
#define IOSERV_OCB_T struct netd_ocb
#define IOSERV_HANDLE_T void

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/iomsg.h>
#include <sys/ioserv.h>
#include <sys/rtmux-core.h>
#include <sys/stat.h>
#include <sys/virtio-mmio-msg.h>
#include <sys/wait.h>
#include <unistd.h>

#include <lkl.h>
#include <lkl_host.h>
#include <lkl/asm-generic/errno.h>
#include <lkl/asm-generic/poll.h>
#include <lkl/asm-generic/socket.h>
#include <lkl/linux/icmp.h>
#include <lkl/linux/in.h>
#include <lkl/linux/time.h>

#include "domain.h"
#include "server.h"
#include "virtio.h"

#define NETD_SLOTS 4

extern char **environ;

static const char *const exports[] = {
    [0] = "/net/ipifc",     [1] = "/net/route", [2] = "/net/dns",
    [3] = "/net/ping",      [4] = "/net/stats", [5] = "/net/tcp/clone",
    [6] = "/net/udp/clone",
};

enum netd_kind {
    NETD_DIR,
    NETD_TEXT,
    NETD_TCP_CLONE,
    NETD_TCP_CTL,
    NETD_TCP_DATA,
    NETD_UDP_CLONE,
    NETD_UDP_CTL,
    NETD_UDP_DATA,
    NETD_DNS,
    NETD_PING,
};

struct netd_ocb {
    enum netd_kind kind;
    int slot;
    unsigned ioflag;
    char text[1024];
    size_t len;
    off_t pos;
    char path[16];
};

static int tcp_fd[NETD_SLOTS] = {
    [0] = -1,
    [1] = -1,
    [2] = -1,
    [3] = -1,
};
static int udp_fd[NETD_SLOTS] = {
    [0] = -1,
    [1] = -1,
    [2] = -1,
    [3] = -1,
};
static pid_t tcp_owner[NETD_SLOTS];
static pid_t udp_owner[NETD_SLOTS];
static unsigned dns_addr[5] = {
    [0] = 10, [1] = 0, [2] = 2, [3] = 3, [4] = 53,
};
static bool crash_on_ping;
static dispatch_t *netd_dpp;
static int netd_path_id = -1;

struct lkl_linger0 {
    int l_onoff;
    int l_linger;
};

static int start_lkl_stack(void)
{
    return rtmux_lkl_start_kernel("lkl-netd", "Linux TCP/IP stack",
                                  RTMUX_LKL_NETD_CMDLINE);
}

static uint16_t be16(uint16_t v)
{
    return (uint16_t) ((v << 8) | (v >> 8));
}

static unsigned ipv4(unsigned a, unsigned b, unsigned c, unsigned d)
{
    unsigned char bytes[4];
    unsigned out;

    bytes[0] = (unsigned char) a;
    bytes[1] = (unsigned char) b;
    bytes[2] = (unsigned char) c;
    bytes[3] = (unsigned char) d;
    memcpy(&out, bytes, sizeof(out));
    return out;
}

static uint16_t inet_cksum(const void *data, size_t len)
{
    const unsigned char *p = data;
    unsigned sum = 0;

    while (len > 1) {
        sum += ((unsigned) p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len != 0)
        sum += (unsigned) p[0] << 8;
    while ((sum >> 16) != 0)
        sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t) ~sum;
}

static int configure_net(int id)
{
    int ifindex;
    int ret;

    rtmux_lkl_status("lkl-netd: query ifindex\n");
    ifindex = lkl_netdev_get_ifindex(id);
    if (ifindex < 0) {
        fprintf(stderr, "lkl-netd: lkl_netdev_get_ifindex: %s\n",
                lkl_strerror(ifindex));
        return ifindex;
    }
    fprintf(stderr, "lkl-netd: ifindex=%d\n", ifindex);
    rtmux_lkl_status("lkl-netd: if addr\n");
    ret = lkl_if_set_ipv4(ifindex, ipv4(10, 0, 2, 15), 24);
    if (ret < 0) {
        fprintf(stderr, "lkl-netd: lkl_if_set_ipv4: %s\n", lkl_strerror(ret));
        return ret;
    }
    rtmux_lkl_status("lkl-netd: if up\n");
    ret = lkl_if_up(ifindex);
    if (ret < 0) {
        fprintf(stderr, "lkl-netd: lkl_if_up: %s\n", lkl_strerror(ret));
        return ret;
    }
    rtmux_lkl_status("lkl-netd: route\n");
    ret = lkl_set_ipv4_gateway(ipv4(10, 0, 2, 2));
    if (ret < 0) {
        fprintf(stderr, "lkl-netd: lkl_set_ipv4_gateway: %s\n",
                lkl_strerror(ret));
        return ret;
    }
    rtmux_lkl_status("lkl-netd: if0 10.0.2.15/24 gw 10.0.2.2\n");
    return 0;
}

static void crash_after_start(void)
{
    volatile int *p = (volatile int *) 0;

    rtmux_lkl_status("lkl-netd: intentional crash after start\n");
    *p = 1;
}

static void slot_close(int *slots, pid_t *owners, int slot)
{
    if (slot < 0 || slot >= NETD_SLOTS)
        return;
    if (slots[slot] >= 0) {
        (void) lkl_sys_shutdown(slots[slot], 2);
        lkl_sys_close((unsigned int) slots[slot]);
    }
    slots[slot] = -1;
    owners[slot] = 0;
}

static int owner_live(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == EOK;
}

static int slot_alloc(int *slots, pid_t *owners, pid_t owner)
{
    for (int i = 0; i < NETD_SLOTS; i++) {
        if (slots[i] != -1 && owners[i] != 0 && !owner_live(owners[i]))
            slot_close(slots, owners, i);
        if (slots[i] == -1) {
            slots[i] = -2;
            owners[i] = owner;
            return i;
        }
    }
    return -1;
}

/* Ownership is by raw pid; orphaned slots are reclaimed lazily in slot_alloc()
 * (and eagerly via close_ocb when the owner dies holding the data OCB).
 * Deliberately no liveness probe on this hot path, since it would let any
 * caller force a /proc syscall storm on the single dispatch thread without even
 * closing the pid-recycling window (a recycled pid equals the owner). Upgrade
 * path: hand out a non-guessable slot handle from clone instead of a 0..N
 * index.
 */
static int slot_check(const int *slots,
                      const pid_t *owners,
                      int slot,
                      pid_t pid)
{
    if (slot < 0 || slot >= NETD_SLOTS)
        return EINVAL;
    if (owners[slot] == 0 && slots[slot] == -1)
        return ENOENT;
    if (owners[slot] != pid)
        return EACCES;
    return EOK;
}

static int socket_connect(int type,
                          int *slots,
                          int slot,
                          unsigned a,
                          unsigned b,
                          unsigned c,
                          unsigned d,
                          unsigned port)
{
    struct lkl_sockaddr_in addr;
    struct lkl_timeval tv = {.tv_sec = 3, .tv_usec = 0};
    long fd;
    long ret;

    if (slot < 0 || slot >= NETD_SLOTS || port > 65535)
        return EINVAL;
    if (slots[slot] >= 0)
        lkl_sys_close((unsigned int) slots[slot]);
    slots[slot] = -2;
    fd = lkl_sys_socket(LKL_AF_INET, type, 0);
    if (fd < 0) {
        slots[slot] = -1;
        return EIO;
    }
    if (type == LKL_SOCK_DGRAM) {
        ret = lkl_sys_setsockopt((int) fd, LKL_SOL_SOCKET, LKL_SO_RCVTIMEO,
                                 (char *) &tv, sizeof(tv));
        if (ret < 0)
            goto fail;
    } else {
        struct lkl_linger0 linger = {.l_onoff = 1, .l_linger = 0};

        ret = lkl_sys_setsockopt((int) fd, LKL_SOL_SOCKET, LKL_SO_LINGER,
                                 (char *) &linger, sizeof(linger));
        if (ret < 0)
            goto fail;
        ret = lkl_sys_setsockopt((int) fd, LKL_SOL_SOCKET, LKL_SO_RCVTIMEO,
                                 (char *) &tv, sizeof(tv));
        if (ret < 0)
            goto fail;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = LKL_AF_INET;
    addr.sin_port = be16((uint16_t) port);
    addr.sin_addr.lkl_s_addr = ipv4(a, b, c, d);
    ret =
        lkl_sys_connect((int) fd, (struct lkl_sockaddr *) &addr, sizeof(addr));
    if (ret < 0)
        goto fail;
    slots[slot] = (int) fd;
    return EOK;

fail:
    lkl_sys_close((unsigned int) fd);
    slots[slot] = -1;
    return lkl_errno(ret);
}

static int tcp_connect(unsigned a,
                       unsigned b,
                       unsigned c,
                       unsigned d,
                       unsigned port,
                       int slot)
{
    return socket_connect(LKL_SOCK_STREAM, tcp_fd, slot, a, b, c, d, port);
}

static int udp_connect(unsigned a,
                       unsigned b,
                       unsigned c,
                       unsigned d,
                       unsigned port,
                       int slot)
{
    return socket_connect(LKL_SOCK_DGRAM, udp_fd, slot, a, b, c, d, port);
}

static int parse_connect(const char *cmd, unsigned out[5])
{
    const char *p = cmd;
    char *end;

    if (strncmp(p, "connect ", 8) != 0)
        return -1;
    p += 8;
    for (int i = 0; i < 5; i++) {
        unsigned long v;

        errno = 0;
        v = strtoul(p, &end, 10);
        if (errno != 0 || end == p || v > (i == 4 ? 65535UL : 255UL))
            return -1;
        out[i] = (unsigned) v;
        if (i < 3) {
            if (*end != '.')
                return -1;
            p = end + 1;
        } else if (i == 3) {
            if (*end != ' ')
                return -1;
            p = end + 1;
        } else {
            while (*end == '\n' || *end == '\r')
                end++;
            if (*end != '\0')
                return -1;
        }
    }
    return 0;
}

static int parse_nameserver(const char *cmd, unsigned out[5])
{
    const char *p = cmd;
    char *end;

    if (strncmp(p, "nameserver ", 11) == 0)
        p += 11;
    for (int i = 0; i < 5; i++) {
        unsigned long v;

        if (i == 4 && (*p == '\0' || *p == '\n' || *p == '\r')) {
            out[i] = 53;
            break;
        }
        errno = 0;
        v = strtoul(p, &end, 10);
        if (errno != 0 || end == p || v > (i == 4 ? 65535UL : 255UL))
            return -1;
        out[i] = (unsigned) v;
        if (i < 3) {
            if (*end != '.')
                return -1;
            p = end + 1;
        } else if (i == 3) {
            if (*end == '\0' || *end == '\n' || *end == '\r') {
                out[4] = 53;
                break;
            }
            if (*end != ' ')
                return -1;
            p = end + 1;
        } else {
            while (*end == '\n' || *end == '\r')
                end++;
            if (*end != '\0')
                return -1;
        }
    }
    return 0;
}

static int parse_ping_line(const char *cmd, unsigned out[4], unsigned *timeout)
{
    const char *p = cmd;
    char *end;

    for (int i = 0; i < 4; i++) {
        unsigned long v;

        errno = 0;
        v = strtoul(p, &end, 10);
        if (errno != 0 || end == p || v > 255UL)
            return -1;
        out[i] = (unsigned) v;
        if (i < 3) {
            if (*end != '.')
                return -1;
            p = end + 1;
        } else {
            p = end;
        }
    }
    while (*p == ' ' || *p == '\t')
        p++;
    *timeout = 3;
    if (*p != '\0' && *p != '\n' && *p != '\r') {
        unsigned long v;

        errno = 0;
        v = strtoul(p, &end, 10);
        if (errno != 0 || end == p || v == 0 || v > 30UL)
            return -1;
        *timeout = (unsigned) v;
        p = end;
    }
    while (*p == '\n' || *p == '\r')
        p++;
    return *p == '\0' ? 0 : -1;
}

static int icmp_ping(unsigned a,
                     unsigned b,
                     unsigned c,
                     unsigned d,
                     unsigned timeout,
                     char *out,
                     size_t outsz)
{
    struct lkl_sockaddr_in addr;
    struct lkl_pollfd pfd;
    struct lkl_timeval tv;
    unsigned char pkt[sizeof(struct lkl_icmphdr) + 56];
    unsigned char rx[128];
    struct lkl_icmphdr *icmp = (struct lkl_icmphdr *) (void *) pkt;
    long fd;
    long ret;
    size_t off;
    unsigned ttl = 64;

    fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_RAW, LKL_IPPROTO_ICMP);
    if (fd < 0)
        return (int) fd;
    tv.tv_sec = (long) timeout;
    tv.tv_usec = 0;
    ret = lkl_sys_setsockopt((int) fd, LKL_SOL_SOCKET, LKL_SO_RCVTIMEO,
                             (char *) &tv, sizeof(tv));
    if (ret < 0) {
        lkl_sys_close((unsigned int) fd);
        return (int) ret;
    }
    memset(pkt, 0, sizeof(pkt));
    icmp->type = LKL_ICMP_ECHO;
    icmp->un.echo.id = be16(0x5254);
    icmp->un.echo.sequence = be16(1);
    memset(pkt + sizeof(*icmp), 0xa5, sizeof(pkt) - sizeof(*icmp));
    icmp->checksum = be16(inet_cksum(pkt, sizeof(pkt)));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = LKL_AF_INET;
    addr.sin_addr.lkl_s_addr = ipv4(a, b, c, d);
    ret = lkl_sys_sendto((int) fd, pkt, sizeof(pkt), 0,
                         (struct lkl_sockaddr *) &addr, sizeof(addr));
    if (ret < 0) {
        lkl_sys_close((unsigned int) fd);
        return (int) ret;
    }
    pfd.fd = (int) fd;
    pfd.events = LKL_POLLIN;
    pfd.revents = 0;
    ret = lkl_sys_poll(&pfd, 1, (int) timeout * 1000);
    if (ret <= 0) {
        lkl_sys_close((unsigned int) fd);
        return ret == 0 ? -LKL_EIO : (int) ret;
    }
    if ((pfd.revents & LKL_POLLIN) == 0) {
        lkl_sys_close((unsigned int) fd);
        return -LKL_EIO;
    }
    ret = lkl_sys_recvfrom((int) fd, rx, sizeof(rx), 0, NULL, NULL);
    lkl_sys_close((unsigned int) fd);
    if (ret < 0)
        return (int) ret;

    off = 0;
    if (ret >= 20 && (rx[0] >> 4) == 4) {
        ttl = rx[8];
        off = (size_t) (rx[0] & 0x0fU) * 4U;
    }
    if (off + sizeof(struct lkl_icmphdr) > (size_t) ret)
        return -LKL_EPROTO;
    icmp = (struct lkl_icmphdr *) (void *) (rx + off);
    if (icmp->type != LKL_ICMP_ECHOREPLY || icmp->un.echo.id != be16(0x5254) ||
        icmp->un.echo.sequence != be16(1))
        return -LKL_EPROTO;
    snprintf(out, outsz, "%ld %u %u.%u.%u.%u\n", ret - (long) off, ttl, a, b, c,
             d);
    return 0;
}

static int parse_slot_path(const char *path,
                           const char *proto,
                           const char *leaf,
                           int *slot)
{
    const char *p = path;
    char *end;
    unsigned long v;

    if (strncmp(p, proto, strlen(proto)) != 0)
        return 0;
    p += strlen(proto);
    if (*p++ != '/')
        return 0;
    errno = 0;
    v = strtoul(p, &end, 10);
    if (errno != 0 || end == p || v >= NETD_SLOTS || *end++ != '/')
        return 0;
    if (strcmp(end, leaf) != 0)
        return 0;
    *slot = (int) v;
    return 1;
}

static int netd_readdir(ioserv_context_t *ctp,
                        io_read_t *msg,
                        struct netd_ocb *ocb)
{
    static const char *const names[] = {
        [0] = "dns",   [1] = "ipifc", [2] = "ping", [3] = "route",
        [4] = "stats", [5] = "tcp",   [6] = "udp",
    };
    struct dirent *dp = (struct dirent *) (void *) msg;
    size_t max_bytes;
    size_t used = 0;

    if (msg->i.nbytes < 0)
        return EINVAL;
    max_bytes = (size_t) msg->i.nbytes;
    if (max_bytes > ctp->msg_max_size)
        max_bytes = ctp->msg_max_size;
    while (ocb->pos < (off_t) (sizeof(names) / sizeof(names[0]))) {
        const char *name = names[ocb->pos];
        size_t namelen = strlen(name);
        size_t reclen = offsetof(struct dirent, d_name) + namelen + 1;
        struct dirent *ep;

        reclen = lib_align_up_size(reclen, 8);
        if (used + reclen > max_bytes) {
            if (used == 0)
                return EINVAL;
            break;
        }
        ep = (struct dirent *) ((char *) dp + used);
        memset(ep, 0, reclen);
        ep->d_ino = (ino_t) (ocb->pos + 1);
        ep->d_offset = ocb->pos + 1;
        ep->d_reclen = (uint16_t) reclen;
        ep->d_namelen = (uint16_t) namelen;
        memcpy(ep->d_name, name, namelen + 1);
        used += reclen;
        ocb->pos++;
    }
    _IO_SET_READ_NBYTES(ctp, used);
    return used > 0 ? IOSERV_PTR(ctp, dp, used) : EOK;
}

static int netd_open(ioserv_context_t *ctp,
                     io_open_t *msg,
                     void *handle,
                     void *extra)
{
    struct netd_ocb *ocb;
    int slot = -1;
    int ret;

    (void) handle;
    (void) extra;
    if (!rtmux_lkl_connect_path_ok((size_t) ctp->size, &msg->connect))
        return EINVAL;
    ret = rtmux_lkl_admission_lock(ctp, msg, NULL);
    if (ret != EOK)
        return ret;
    ocb = calloc(1, sizeof(*ocb));
    if (!ocb) {
        rtmux_lkl_admission_unlock(ctp, msg, NULL);
        return ENOMEM;
    }
    ocb->ioflag = msg->connect.ioflag;
    if (msg->connect.path[0] == '\0' || !strcmp(msg->connect.path, "tcp") ||
        !strcmp(msg->connect.path, "udp")) {
        ocb->kind = NETD_DIR;
        (void) snprintf(ocb->path, sizeof ocb->path, "/net%s%s",
                        msg->connect.path[0] ? "/" : "", msg->connect.path);
    } else if (!strcmp(msg->connect.path, "tcp/clone")) {
        /* clone exists to be read for the slot id; a write-only open can't do
         * that, so don't let it tie up one of NETD_SLOTS slots. A live client
         * can still leak slots by opening clone (each reserves a slot) without
         * ever writing "close" to the ctl node. Only 4 slots exist; they're
         * reclaimed when the owner dies (close_ocb / slot_alloc's dead-owner
         * sweep) but not while it lives. Upgrade path if this matters: refcount
         * connections with an idle timeout, or free the slot when the clone fd
         * closes unconnected (needs the client to hold clone open like Plan9
         * ctl, which net_clone_id currently doesn't).
         */
        if (!(msg->connect.ioflag & _IO_FLAG_RD)) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return EBADF;
        }
        slot = slot_alloc(tcp_fd, tcp_owner, ctp->info.pid);
        if (slot < 0) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return EBUSY;
        }
        ocb->kind = NETD_TCP_CLONE;
        ocb->slot = slot;
        ocb->len =
            rtmux_lkl_text_format(ocb->text, sizeof(ocb->text), "%d\n", slot);
    } else if (parse_slot_path(msg->connect.path, "tcp", "ctl", &slot)) {
        int ret = slot_check(tcp_fd, tcp_owner, slot, ctp->info.pid);

        if (ret != EOK) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return ret;
        }
        ocb->kind = NETD_TCP_CTL;
        ocb->slot = slot;
        ocb->len =
            rtmux_lkl_text_format(ocb->text, sizeof(ocb->text), "closed\n");
    } else if (parse_slot_path(msg->connect.path, "tcp", "data", &slot)) {
        int ret = slot_check(tcp_fd, tcp_owner, slot, ctp->info.pid);

        if (ret != EOK) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return ret;
        }
        ocb->kind = NETD_TCP_DATA;
        ocb->slot = slot;
    } else if (!strcmp(msg->connect.path, "udp/clone")) {
        if (!(msg->connect.ioflag & _IO_FLAG_RD)) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return EBADF;
        }
        slot = slot_alloc(udp_fd, udp_owner, ctp->info.pid);
        if (slot < 0) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return EBUSY;
        }
        ocb->kind = NETD_UDP_CLONE;
        ocb->slot = slot;
        ocb->len =
            rtmux_lkl_text_format(ocb->text, sizeof(ocb->text), "%d\n", slot);
    } else if (parse_slot_path(msg->connect.path, "udp", "ctl", &slot)) {
        int ret = slot_check(udp_fd, udp_owner, slot, ctp->info.pid);

        if (ret != EOK) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return ret;
        }
        ocb->kind = NETD_UDP_CTL;
        ocb->slot = slot;
        ocb->len =
            rtmux_lkl_text_format(ocb->text, sizeof(ocb->text), "closed\n");
    } else if (parse_slot_path(msg->connect.path, "udp", "data", &slot)) {
        int ret = slot_check(udp_fd, udp_owner, slot, ctp->info.pid);

        if (ret != EOK) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return ret;
        }
        ocb->kind = NETD_UDP_DATA;
        ocb->slot = slot;
    } else if (!strcmp(msg->connect.path, "ipifc")) {
        if ((msg->connect.ioflag & _IO_FLAG_WR) != 0) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return EROFS;
        }
        ocb->kind = NETD_TEXT;
        ocb->len = rtmux_lkl_text_format(ocb->text, sizeof(ocb->text),
                                         "if0 10.0.2.15/24 up\n");
    } else if (!strcmp(msg->connect.path, "route")) {
        if ((msg->connect.ioflag & _IO_FLAG_WR) != 0) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return EROFS;
        }
        ocb->kind = NETD_TEXT;
        ocb->len = rtmux_lkl_text_format(ocb->text, sizeof(ocb->text),
                                         "default 10.0.2.2\n");
    } else if (!strcmp(msg->connect.path, "stats")) {
        struct rtmux_lkl_host_stats stats;
        char ioserv_stats[256];

        if ((msg->connect.ioflag & _IO_FLAG_WR) != 0) {
            free(ocb);
            rtmux_lkl_admission_unlock(ctp, msg, NULL);
            return EROFS;
        }
        rtmux_lkl_host_stats(&stats);
        (void) ioserv_reply_counters_format(ioserv_stats, sizeof(ioserv_stats));
        ocb->kind = NETD_TEXT;
        ocb->len = rtmux_lkl_text_format(
            ocb->text, sizeof(ocb->text),
            "server lkl-netd\nnetwork ipv4\nlkl_mem %s\n"
            "page_current %llu\npage_high %llu\npage_total %llu\n"
            "dma_current %llu\ndma_high %llu\ndma_total %llu\n"
            "net_bounce_rx_bytes %llu\nnet_bounce_tx_bytes %llu\n%s"
            "lkl_block_read_bytes %llu\nlkl_block_write_bytes %llu\n"
            "lkl_threads %llu\nlkl_thread_stack_bytes %llu\n",
            RTMUX_LKL_NETD_CMDLINE, stats.page_current, stats.page_high,
            stats.page_total, stats.dma_current, stats.dma_high,
            stats.dma_total, stats.net_bounce_rx_bytes,
            stats.net_bounce_tx_bytes, ioserv_stats, stats.lkl_block_read_bytes,
            stats.lkl_block_write_bytes, stats.thread_count,
            stats.thread_stack_bytes);
    } else if (!strcmp(msg->connect.path, "dns")) {
        ocb->kind = NETD_DNS;
        ocb->len = rtmux_lkl_text_format(
            ocb->text, sizeof(ocb->text), "nameserver %u.%u.%u.%u %u\n",
            dns_addr[0], dns_addr[1], dns_addr[2], dns_addr[3], dns_addr[4]);
    } else if (!strcmp(msg->connect.path, "ping")) {
        ocb->kind = NETD_PING;
    } else {
        free(ocb);
        rtmux_lkl_admission_unlock(ctp, msg, NULL);
        return ENOENT;
    }
    if (ocb->len >= sizeof(ocb->text))
        ocb->len = sizeof(ocb->text) - 1;
    if (ioserv_open_bind(ctp, ocb, NULL) == -1) {
        int saved = errno;

        /* A clone reserved a scarce slot above; don't leak it if the bind that
         * would let the client ever close it fails.
         */
        if (ocb->kind == NETD_TCP_CLONE)
            slot_close(tcp_fd, tcp_owner, ocb->slot);
        else if (ocb->kind == NETD_UDP_CLONE)
            slot_close(udp_fd, udp_owner, ocb->slot);
        free(ocb);
        rtmux_lkl_admission_unlock(ctp, msg, NULL);
        return saved;
    }
    rtmux_lkl_admission_unlock(ctp, msg, NULL);
    return EOK;
}

static int netd_read(ioserv_context_t *ctp,
                     io_read_t *msg,
                     struct netd_ocb *ocb)
{
    char *buf = (char *) (void *) msg;
    size_t nbytes;

    if (msg->i.nbytes < 0)
        return EINVAL;
    if (!(ocb->ioflag & _IO_FLAG_RD))
        return EBADF;
    nbytes = (size_t) msg->i.nbytes;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (ocb->kind == NETD_DIR)
        return netd_readdir(ctp, msg, ocb);
    /* buf aliases the receive message; never read past it. */
    if (nbytes > ctp->msg_max_size)
        nbytes = ctp->msg_max_size;
    if (ocb->kind == NETD_TCP_DATA || ocb->kind == NETD_UDP_DATA) {
        long ret;
        int *slots = ocb->kind == NETD_TCP_DATA ? tcp_fd : udp_fd;
        int fd;

        int check = slot_check(
            slots, ocb->kind == NETD_TCP_DATA ? tcp_owner : udp_owner,
            ocb->slot, ctp->info.pid);

        if (check != EOK)
            return check;
        fd = slots[ocb->slot];
        if (fd < 0)
            return ENOTCONN;
        ret = lkl_sys_read((unsigned int) fd, buf, nbytes);
        if (ret < 0)
            return lkl_errno(ret);
        _IO_SET_READ_NBYTES(ctp, (size_t) ret);
        return ret > 0 ? IOSERV_PTR(ctp, buf, (size_t) ret) : EOK;
    }
    if (ocb->kind == NETD_TCP_CTL || ocb->kind == NETD_UDP_CTL) {
        int *slots = ocb->kind == NETD_TCP_CTL ? tcp_fd : udp_fd;
        int fd;

        pid_t *owners = ocb->kind == NETD_TCP_CTL ? tcp_owner : udp_owner;
        int check = slot_check(slots, owners, ocb->slot, ctp->info.pid);

        if (check != EOK)
            return check;
        fd = slots[ocb->slot];
        ocb->len = rtmux_lkl_text_format(ocb->text, sizeof(ocb->text), "%s\n",
                                         fd >= 0 ? "connected" : "closed");
    } else if (ocb->kind == NETD_DNS) {
        ocb->len = rtmux_lkl_text_format(
            ocb->text, sizeof(ocb->text), "nameserver %u.%u.%u.%u %u\n",
            dns_addr[0], dns_addr[1], dns_addr[2], dns_addr[3], dns_addr[4]);
    }
    if (ocb->pos >= (off_t) ocb->len) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }
    if (nbytes > ocb->len - (size_t) ocb->pos)
        nbytes = ocb->len - (size_t) ocb->pos;
    memcpy(buf, ocb->text + ocb->pos, nbytes);
    ocb->pos += (off_t) nbytes;
    _IO_SET_READ_NBYTES(ctp, nbytes);
    return nbytes > 0 ? IOSERV_PTR(ctp, buf, nbytes) : EOK;
}

static int netd_write(ioserv_context_t *ctp,
                      io_write_t *msg,
                      struct netd_ocb *ocb)
{
    size_t nbytes;
    size_t skip = sizeof(msg->i);
    char *buf;

    if (msg->i.nbytes < 0)
        return EINVAL;
    if (!(ocb->ioflag & _IO_FLAG_WR))
        return EBADF;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return ENOSYS;
    if (ctp->msg_max_size < skip)
        return EINVAL;
    nbytes = (size_t) msg->i.nbytes;
    if (nbytes > ctp->msg_max_size - skip)
        nbytes = ctp->msg_max_size - skip;
    if ((size_t) ctp->size < skip + nbytes)
        nbytes = (size_t) ctp->size > skip ? (size_t) ctp->size - skip : 0;
    buf = (char *) (void *) msg + skip;
    if (ocb->kind == NETD_TCP_CTL || ocb->kind == NETD_UDP_CTL) {
        unsigned addr[5];
        char cmd[80];
        int *slots = ocb->kind == NETD_TCP_CTL ? tcp_fd : udp_fd;
        pid_t *owners = ocb->kind == NETD_TCP_CTL ? tcp_owner : udp_owner;
        int ret;

        if (nbytes >= sizeof(cmd))
            return EINVAL;
        ret = slot_check(slots, owners, ocb->slot, ctp->info.pid);
        if (ret != EOK)
            return ret;
        memcpy(cmd, buf, nbytes);
        cmd[nbytes] = '\0';
        if (parse_connect(cmd, addr) == 0) {
            if (ocb->kind == NETD_TCP_CTL)
                ret = tcp_connect(addr[0], addr[1], addr[2], addr[3], addr[4],
                                  ocb->slot);
            else
                ret = udp_connect(addr[0], addr[1], addr[2], addr[3], addr[4],
                                  ocb->slot);

            if (ret != EOK)
                return ret;
        } else if (strncmp(cmd, "close", 5) == 0) {
            if (ocb->kind == NETD_TCP_CTL)
                slot_close(tcp_fd, tcp_owner, ocb->slot);
            else
                slot_close(udp_fd, udp_owner, ocb->slot);
        } else {
            return EINVAL;
        }
        _IO_SET_WRITE_NBYTES(ctp, nbytes);
        return EOK;
    }
    if (ocb->kind == NETD_DNS) {
        unsigned addr[5];
        char cmd[80];

        if (nbytes >= sizeof(cmd))
            return EINVAL;
        memcpy(cmd, buf, nbytes);
        cmd[nbytes] = '\0';
        if (parse_nameserver(cmd, addr) != 0)
            return EINVAL;
        memcpy(dns_addr, addr, sizeof(dns_addr));
        _IO_SET_WRITE_NBYTES(ctp, nbytes);
        return EOK;
    }
    if (ocb->kind == NETD_PING) {
        unsigned addr[4];
        unsigned timeout;
        char cmd[32];
        int ret;

        if (nbytes >= sizeof(cmd))
            return EINVAL;
        memcpy(cmd, buf, nbytes);
        cmd[nbytes] = '\0';
        if (parse_ping_line(cmd, addr, &timeout) != 0)
            return EINVAL;
        if (crash_on_ping) {
            if (netd_dpp && netd_path_id != -1)
                (void) ioserv_detach(netd_dpp, netd_path_id,
                                     IOSERV_DETACH_PATHNAME);
            rtmux_lkl_netdev_crash_after_tx(true);
        }
        ret = icmp_ping(addr[0], addr[1], addr[2], addr[3], timeout, ocb->text,
                        sizeof(ocb->text));
        if (ret < 0)
            return lkl_errno(ret);
        ocb->len = strlen(ocb->text);
        ocb->pos = 0;
        _IO_SET_WRITE_NBYTES(ctp, nbytes);
        return EOK;
    }
    if (ocb->kind == NETD_TCP_DATA || ocb->kind == NETD_UDP_DATA) {
        long ret;
        int *slots = ocb->kind == NETD_TCP_DATA ? tcp_fd : udp_fd;
        int fd;

        pid_t *owners = ocb->kind == NETD_TCP_DATA ? tcp_owner : udp_owner;
        int check = slot_check(slots, owners, ocb->slot, ctp->info.pid);

        if (check != EOK)
            return check;
        fd = slots[ocb->slot];
        if (fd < 0)
            return ENOTCONN;
        ret = lkl_sys_write((unsigned int) fd, buf, nbytes);
        if (ret < 0)
            return lkl_errno(ret);
        _IO_SET_WRITE_NBYTES(ctp, (size_t) ret);
        return EOK;
    }
    return EROFS;
}

static int netd_stat(ioserv_context_t *ctp,
                     io_stat_t *msg,
                     struct netd_ocb *ocb)
{
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.st_mode = ocb->kind == NETD_DIR ? S_IFDIR | 0555
                     : (ocb->kind == NETD_DNS || ocb->kind == NETD_PING)
                         ? S_IFREG | 0666
                         : S_IFREG | 0444;
    msg->o.st_nlink = 1;
    msg->o.st_size = (off_t) ocb->len;
    msg->o.st_blksize = 512;
    return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int netd_close(ioserv_context_t *ctp,
                      void *reserved,
                      struct netd_ocb *ocb)
{
    (void) ctp;
    (void) reserved;
    rtmux_lkl_admission_wait(ctp, NULL, ocb);
    if (ocb->kind == NETD_TCP_DATA)
        slot_close(tcp_fd, tcp_owner, ocb->slot);
    else if (ocb->kind == NETD_UDP_DATA)
        slot_close(udp_fd, udp_owner, ocb->slot);
    free(ocb);
    rtmux_lkl_admission_unlock(ctp, NULL, ocb);
    return EOK;
}

static int netd_fdinfo(ioserv_context_t *ctp,
                       io_fdinfo_t *msg,
                       struct netd_ocb *ocb)
{
    char *path = (char *) (&msg->o + 1);
    size_t cap = (size_t) msg->i.path_len;
    size_t len;

    (void) ctp;
    if (ocb->kind != NETD_DIR)
        return ENOSYS;
    len = strlen(ocb->path) + 1;
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.info.mode = S_IFDIR | 0555;
    if (cap > 0)
        memcpy(path, ocb->path, cap < len ? cap : len);
    _IO_SET_FDINFO_LEN(ctp, len);
    return IOSERV_PTR(ctp, &msg->o,
                      sizeof msg->o + (unsigned) (cap < len ? cap : len));
}

static int netd_admission_lock(ioserv_context_t *ctp,
                               void *msg,
                               struct netd_ocb *ocb)
{
    return rtmux_lkl_admission_lock(ctp, msg, ocb);
}

static int netd_admission_unlock(ioserv_context_t *ctp,
                                 void *msg,
                                 struct netd_ocb *ocb)
{
    return rtmux_lkl_admission_unlock(ctp, msg, ocb);
}

static const ioserv_connect_ops_t netd_connect_funcs = {
    .nfuncs = IOSERV_CONNECT_NFUNCS,
    .open = netd_open,
};

static const ioserv_ops_t netd_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = netd_read,
    .write = netd_write,
    .close_ocb = netd_close,
    .stat = netd_stat,
    .fdinfo = netd_fdinfo,
    .lock_ocb = netd_admission_lock,
    .unlock_ocb = netd_admission_unlock,
};

static int serve_net(void)
{
    dispatch_t *dpp;

    dpp = rtmux_lkl_dispatch_begin("lkl-netd");
    if (!dpp)
        return -1;
    netd_path_id = ioserv_attach(dpp, NULL, "/net", _FTYPE_ANY, IOSERV_FLAG_DIR,
                                 &netd_connect_funcs, &netd_io_funcs, NULL);
    if (netd_path_id == -1) {
        fprintf(stderr, "lkl-netd: attach /net: %s\n", strerror(errno));
        return -1;
    }
    netd_dpp = dpp;
    fprintf(stderr, "lkl-netd: serving /net\n");
    return rtmux_lkl_dispatch_serve(dpp, "lkl-netd");
}

static int delegate_virtio_mmio_device(pid_t pid,
                                       uint32_t device_id,
                                       const char *label,
                                       bool *out_iommu_stream)
{
    struct virtio_mmio_message msg;
    int fd;
    int rc;

    if (out_iommu_stream)
        *out_iommu_stream = false;
    /* ipc-send: lkl-netd -> virtio-mmio */
    fd = open(VIRTIO_MMIO_SERVICE_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "lkl-netd: open %s: %s\n", VIRTIO_MMIO_SERVICE_PATH,
                strerror(errno));
        return -1;
    }
    memset(&msg, 0, sizeof(msg));
    msg.msg_hdr.i.type = _IO_MSG;
    msg.msg_hdr.i.combine_len = sizeof(msg);
    msg.msg_hdr.i.protocol = _IOMSG_VIRTIO_MMIO;
    msg.msg_hdr.i.subtype = IOM_VIRTIO_MMIO_DELEGATE;
    msg.msg.delegate.device_id = device_id;
    msg.msg.delegate.max_handles = VIRTIO_MMIO_CAND_MAX;
    msg.msg.delegate.pid = pid;
    /* ipc-send: lkl-netd -> virtio-mmio */
    rc = msg_send(fd, &msg, sizeof(msg), &msg, sizeof(msg));
    close(fd);
    if (rc == -1) {
        fprintf(stderr, "lkl-netd: delegate %s to pid %d: %s\n", label,
                (int) pid, strerror(errno));
        return -1;
    }
    if (msg.rep_hdr.reply_status != EOK) {
        errno = (int) msg.rep_hdr.reply_status;
        fprintf(stderr, "lkl-netd: delegate %s to pid %d: %s\n", label,
                (int) pid, strerror(errno));
        return -1;
    }
    if (msg.rep_hdr.reply_count == 0) {
        errno = ENODEV;
        fprintf(stderr, "lkl-netd: no %s device to delegate\n", label);
        return -1;
    }
    if (out_iommu_stream) {
        for (uint32_t i = 0; i < msg.rep_hdr.reply_count; i++) {
            if (msg.handles[i].reserved != RTMUX_RESOURCE_IOMMU_STREAM_NONE)
                *out_iommu_stream = true;
        }
    }
    return 0;
}

static int request_virtio_mmio_delegation(pid_t pid)
{
    bool needs_iommu = false;

    if (delegate_virtio_mmio_device(pid, VIRTIO_ID_NET, "virtio-mmio net",
                                    &needs_iommu) == -1)
        return -1;
    if (needs_iommu &&
        delegate_virtio_mmio_device(pid, RTMUX_DEVICE_RISCV_IOMMU,
                                    "system IOMMU", NULL) == -1)
        return -1;
    return 0;
}

static int wait_child(pid_t pid)
{
    int status;

    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR)
            return EXIT_FAILURE;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return EXIT_FAILURE;
}

static int delegate_netd_child(const char *child_arg)
{
    char *child_argv[] = {
        "lkl-netd",
        "--delegated-virtio",
        (char *) child_arg,
        NULL,
    };
    pid_t pid;
    int rc;

    rc = posix_spawn(&pid, "/sbin/lkl-netd", NULL, NULL, child_argv, environ);
    if (rc != EOK) {
        fprintf(stderr, "lkl-netd: spawn delegated child: %s\n", strerror(rc));
        return EXIT_FAILURE;
    }
    if (request_virtio_mmio_delegation(pid) == -1) {
        (void) kill(pid, SIGKILL);
        (void) wait_child(pid);
        return EXIT_FAILURE;
    }
    return wait_child(pid);
}

int main(int argc, char **argv)
{
    struct lkl_netdev_args args;
    struct lkl_netdev *nd;
    int id;
    int ret;
    bool boot_only = false;
    bool crash = false;
    bool serve_files = false;
    bool delegated_child = false;
    int argi = 1;

    /* Public entry points: each re-execs a --delegated-virtio child that owns
     * the MMIO handoff, then serves. --serve-net{,-crash-on-ping} take the
     * clean name, but the boot-only/crash-after-start delegators keep the
     * --delegate- prefix on purpose: the bare --boot-only / --crash-after-start
     * names are the direct, non-delegated path parsed below, which the
     * lkl-netd-direct-denied regress relies on. Don't "unify" the names.
     */
    if (argc == 2 && !strcmp(argv[1], "--serve-net"))
        return delegate_netd_child("--serve-net");
    if (argc == 2 && !strcmp(argv[1], "--serve-net-crash-on-ping"))
        return delegate_netd_child("--serve-net-crash-on-ping");
    if (argc == 2 && !strcmp(argv[1], "--delegate-crash-after-start"))
        return delegate_netd_child("--crash-after-start");
    if (argc == 2 && !strcmp(argv[1], "--delegate-boot-only"))
        return delegate_netd_child("--boot-only");

    if (argc > 1 && !strcmp(argv[1], "--delegated-virtio")) {
        delegated_child = true;
        argi = 2;
    }

    if (argc == argi + 1 && !strcmp(argv[argi], "--serve-net"))
        serve_files = true;
    else if (argc == argi + 1 &&
             !strcmp(argv[argi], "--serve-net-crash-on-ping")) {
        serve_files = true;
        crash_on_ping = true;
    } else if (argc == argi + 1 && !strcmp(argv[argi], "--crash-after-start"))
        crash = true;
    else if (argc == argi + 1 && !strcmp(argv[argi], "--boot-only"))
        boot_only = true;
    else if (argc > argi) {
        fprintf(stderr,
                "usage: lkl-netd "
                "[--serve-net|--serve-net-crash-on-ping|--boot-only|"
                "--crash-after-start|--delegate-crash-after-start|"
                "--delegate-boot-only]\n");
        return EXIT_FAILURE;
    }
    if (delegated_child)
        rtmux_lkl_netdev_set_delegated_mmio(true);

    rtmux_lkl_status("lkl-netd: init\n");
    ret = lkl_init(&lkl_host_ops);
    if (ret < 0) {
        fprintf(stderr, "lkl-netd: lkl_init: %s\n", lkl_strerror(ret));
        return EXIT_FAILURE;
    }

    if (boot_only) {
        ret = start_lkl_stack();
        lkl_cleanup();
        return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    rtmux_lkl_status("lkl-netd: netdev\n");
    nd = rtmux_lkl_netdev_create();
    if (!nd) {
        fputs("lkl-netd: could not create RTMux netdev backend\n", stderr);
        lkl_cleanup();
        return EXIT_FAILURE;
    }

    memset(&args, 0, sizeof(args));
    rtmux_lkl_status("lkl-netd: virtio-net add\n");
    id = lkl_netdev_add(nd, &args);
    if (id < 0) {
        fprintf(stderr, "lkl-netd: lkl_netdev_add: %s\n", lkl_strerror(id));
        rtmux_lkl_netdev_destroy(nd);
        lkl_cleanup();
        return EXIT_FAILURE;
    }
    nd = NULL; /* lkl_netdev_remove(id) now owns the backend free path. */
    if (start_lkl_stack() < 0) {
        lkl_netdev_remove(id);
        lkl_cleanup();
        return EXIT_FAILURE;
    }
    ret = configure_net(id);
    if (ret < 0) {
        lkl_netdev_remove(id);
        lkl_cleanup();
        return EXIT_FAILURE;
    }

    /* All virtio-net MMIO is mapped and the stack is up; seal physical-mapping
     * rights so a later bug in request handling cannot reach new device
     * registers. DMA buffers are MAP_ANON and stay allowed. Irreversible.
     */
    if (rtmux_phys_contain() != EOK) {
        fprintf(stderr, "lkl-netd: rtmux_phys_contain: %s\n", strerror(errno));
        lkl_netdev_remove(id);
        lkl_cleanup();
        return EXIT_FAILURE;
    }
    if (crash)
        crash_after_start();
    if (serve_files)
        return serve_net() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    /* Bring-up smoke only: the Linux stack and QEMU packet backend are up. */
    rtmux_lkl_status(
        "lkl-netd: bring-up smoke OK (stack and packet backend)\n");
    rtmux_lkl_status("lkl-netd: export surface:\n");
    for (size_t i = 0; i < sizeof(exports) / sizeof(exports[0]); i++)
        puts(exports[i]);
    lkl_netdev_remove(id);
    lkl_cleanup();
    return EXIT_SUCCESS;
}
