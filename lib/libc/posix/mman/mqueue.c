/*
 * POSIX message queues, backed by a shared-memory object.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A queue is a single shm object holding a header (its mq_attr plus a
 * process-shared spin lock) followed by a fixed ring of message slots. This
 * reuses existing IPC — shm objects and process-shared spin locks — rather than
 * a dedicated server, per the §12 "use existing IPC where smaller" guidance.
 * mqd_t is the shm object's descriptor.
 *
 * The queue name lives in its own namespace: an mq name "/x" maps to the shm
 * object "/mq.x", so it does not collide with a plain shm_open("/x"). The
 * creator sizes and initializes the object, then publishes a ready magic with a
 * release store; a concurrent opener waits (acquire) for that magic before
 * using the object, closing the create-vs-open race.
 *
 * A queue's O_NONBLOCK flag and access mode belong to the open descriptor, not
 * the shared object (POSIX): two processes can open the same queue read-only
 * and write-only. A process-private descriptor table records each mqd_t's open
 * flags so mq_send/mq_receive can reject the wrong access mode (EBADF) and
 * consult the per-descriptor O_NONBLOCK.
 *
 * Blocking (plain mq_send/mq_receive, or the timed variants without O_NONBLOCK)
 * is a bounded busy-wait: there is no process-shared condition variable, so a
 * blocked caller drops the lock, yields, and re-checks until the queue has room
 * (send) or a message (receive), or the absolute deadline passes (ETIMEDOUT);
 * an untimed call blocks indefinitely. This spends CPU while blocked; queues
 * are meant for low-rate control messages, not high-throughput streaming.
 *
 * Two limitations follow from that design and are accepted here: the shared
 * spin lock is not robust — a process killed while holding it wedges the queue
 * — and, because the operations take that lock (and the descriptor-table
 * mutex), they are not async-signal-safe despite POSIX listing mq_send/
 * mq_receive as such. Both would need a kernel-mediated queue object to fix.
 */

#include <mqueue.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MQ_DEFAULT_MAXMSG 10
#define MQ_DEFAULT_MSGSIZE 8192
#define MQ_MAXMSG_LIMIT 1024
#define MQ_MSGSIZE_LIMIT (1024 * 1024)
#define MQ_MAGIC 0x716d7551u /* "mqQ" — set last once the queue is usable */
#define MQ_READY_SPINS 100000
/* priorities are 0 .. MQ_PRIO_MAX-1; MQ_PRIO_MAX comes from <limits.h> */
#define MQ_DESC_MAX 64 /* open mq descriptors tracked per process */

/* One message slot: a small header immediately followed by mq_msgsize bytes. */
struct mq_slot {
    long mq_prio;
    unsigned long mq_seq; /* insertion order, for FIFO within equal priority */
    size_t mq_len;
    int mq_used;
};

struct mq_shm {
    _Atomic unsigned
        magic; /* MQ_MAGIC once initialized; first for early read */
    struct mq_attr attr;
    pthread_spinlock_t lock;
    unsigned long seq_next; /* next mq_send sequence number */
    /* `attr.mq_maxmsg` slots follow, each sizeof(struct mq_slot)+mq_msgsize. */
};

/* Round up to 8-byte alignment so each slot (and the trailing data) stays
 * naturally aligned.
 */
static size_t mq_align(size_t n)
{
    return (n + 7u) & ~(size_t) 7u;
}

static size_t mq_slot_stride(long msgsize)
{
    return mq_align(sizeof(struct mq_slot) + (size_t) msgsize);
}

static size_t mq_object_size(long maxmsg, long msgsize)
{
    return mq_align(sizeof(struct mq_shm)) +
           (size_t) maxmsg * mq_slot_stride(msgsize);
}

/* Process-private descriptor table: an mqd_t is a plain read-write shm fd, so
 * the open flags (access mode + O_NONBLOCK) live here, not in the shared
 * object. Guarded by a mutex; the table is small and touched only at
 * open/close/get/setattr.
 */
static struct mq_desc {
    int fd; /* -1 when the slot is free */
    int flags;
} mq_descs[MQ_DESC_MAX];
static pthread_mutex_t mq_desc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t mq_desc_once = PTHREAD_ONCE_INIT;

/* fd 0 is a valid descriptor, so an all-zero table would alias it; mark every
 * slot free (-1) once before first use.
 */
static void mq_desc_setup(void)
{
    for (int i = 0; i < MQ_DESC_MAX; i++)
        mq_descs[i].fd = -1;
}

static int mq_desc_add(int fd, int flags)
{
    int rc = -1;

    pthread_once(&mq_desc_once, mq_desc_setup);
    pthread_mutex_lock(&mq_desc_lock);
    for (int i = 0; i < MQ_DESC_MAX; i++) {
        if (mq_descs[i].fd < 0) {
            mq_descs[i].fd = fd;
            mq_descs[i].flags = flags;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&mq_desc_lock);
    if (rc == -1)
        errno = EMFILE;
    return rc;
}

/* Free a descriptor's slot; returns 1 if it was one of ours, else 0. */
static int mq_desc_del(int fd)
{
    int found = 0;

    pthread_once(&mq_desc_once, mq_desc_setup);
    pthread_mutex_lock(&mq_desc_lock);
    for (int i = 0; i < MQ_DESC_MAX; i++) {
        if (mq_descs[i].fd == fd) {
            mq_descs[i].fd = -1;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&mq_desc_lock);
    return found;
}

/* Fetch a descriptor's flags; -1 (EBADF) if it is not one of ours. */
static int mq_desc_flags(int fd)
{
    int flags = -1;

    pthread_once(&mq_desc_once, mq_desc_setup);
    pthread_mutex_lock(&mq_desc_lock);
    for (int i = 0; i < MQ_DESC_MAX; i++) {
        if (mq_descs[i].fd == fd) {
            flags = mq_descs[i].flags;
            break;
        }
    }
    pthread_mutex_unlock(&mq_desc_lock);
    if (flags == -1)
        errno = EBADF;
    return flags;
}

/* Replace a descriptor's O_NONBLOCK bit, returning the old flags (or -1). */
static int mq_desc_setblock(int fd, int nonblock)
{
    int old = -1;

    pthread_once(&mq_desc_once, mq_desc_setup);
    pthread_mutex_lock(&mq_desc_lock);
    for (int i = 0; i < MQ_DESC_MAX; i++) {
        if (mq_descs[i].fd == fd) {
            old = mq_descs[i].flags;
            mq_descs[i].flags =
                (old & ~O_NONBLOCK) | (nonblock ? O_NONBLOCK : 0);
            break;
        }
    }
    pthread_mutex_unlock(&mq_desc_lock);
    if (old == -1)
        errno = EBADF;
    return old;
}

/* A message-queue name must be "/" followed by up to NAME_MAX non-slash
 * characters (POSIX).
 */
static int mq_name_ok(const char *name)
{
    if (!name || name[0] != '/')
        return 0;
    if (name[1] == '\0' || strchr(name + 1, '/'))
        return 0;
    return strlen(name + 1) <= NAME_MAX;
}

/* Map an mq name to its private shm object name, "/mq.<name>". */
static int mq_shm_name(const char *name, char *buf, size_t buflen)
{
    int n = snprintf(buf, buflen, "/mq.%s", name + 1);
    if (n < 0 || (size_t) n >= buflen) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* Sizes and initializes a freshly-created queue object, publishing MQ_MAGIC
 * last so an opener never observes a half-built queue.
 */
static int mq_init(int fd, const struct mq_attr *attr)
{
    struct mq_shm *hdr;
    long maxmsg = attr && attr->mq_maxmsg ? attr->mq_maxmsg : MQ_DEFAULT_MAXMSG;
    long msgsize =
        attr && attr->mq_msgsize ? attr->mq_msgsize : MQ_DEFAULT_MSGSIZE;
    size_t hdrmap = mq_align(sizeof(struct mq_shm));
    int rc;

    if (maxmsg <= 0 || maxmsg > MQ_MAXMSG_LIMIT || msgsize <= 0 ||
        msgsize > MQ_MSGSIZE_LIMIT) {
        errno = EINVAL;
        return -1;
    }

    flock_t space = {.l_whence = SEEK_SET,
                     .l_len = (off_t) mq_object_size(maxmsg, msgsize)};
    if (fcntl(fd, F_ALLOCSP, &space) == -1)
        return -1;

    /* Only the header is touched here; the ring stays demand-zeroed. */
    hdr = mmap(NULL, hdrmap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (hdr == MAP_FAILED)
        return -1;

    memset(hdr, 0, hdrmap);
    hdr->attr.mq_maxmsg = maxmsg;
    hdr->attr.mq_msgsize = msgsize;
    hdr->attr.mq_curmsgs = 0;
    if ((rc = pthread_spin_init(&hdr->lock, PTHREAD_PROCESS_SHARED)) != 0) {
        (void) munmap(hdr, hdrmap);
        errno = rc;
        return -1;
    }
    atomic_store_explicit(&hdr->magic, MQ_MAGIC, memory_order_release);
    (void) munmap(hdr, hdrmap);
    return 0;
}

/* Wait for a concurrently-created queue to become usable: the object must be at
 * least header-sized and its magic published.
 */
static int mq_wait_ready(int fd)
{
    size_t hdrmap = mq_align(sizeof(struct mq_shm));

    for (int i = 0; i < MQ_READY_SPINS; i++) {
        struct stat st;

        if (fstat(fd, &st) == 0 &&
            (size_t) st.st_size >= sizeof(struct mq_shm)) {
            struct mq_shm *hdr =
                mmap(NULL, hdrmap, PROT_READ, MAP_SHARED, fd, 0);
            if (hdr != MAP_FAILED) {
                unsigned m =
                    atomic_load_explicit(&hdr->magic, memory_order_acquire);
                (void) munmap(hdr, hdrmap);
                if (m == MQ_MAGIC)
                    return 0;
            }
        }
        sched_yield();
    }
    errno = EAGAIN;
    return -1;
}

/* Record the open descriptor's flags, closing the fd if the table is full. */
static mqd_t mq_finish(int fd, int oflag)
{
    if (mq_desc_add(fd, oflag) == -1) {
        int e = errno;
        (void) close(fd);
        errno = e;
        return (mqd_t) -1;
    }
    return (mqd_t) fd;
}

mqd_t mq_open(const char *name, int oflag, ...)
{
    char shm[NAME_MAX + 8];
    int fd;

    if (!mq_name_ok(name)) {
        errno = EINVAL;
        return (mqd_t) -1;
    }
    if (mq_shm_name(name, shm, sizeof shm) == -1)
        return (mqd_t) -1;

    if (oflag & O_CREAT) {
        va_list ap;
        mode_t mode;
        const struct mq_attr *attr;

        va_start(ap, oflag);
        mode = (mode_t) va_arg(ap, unsigned);
        attr = va_arg(ap, const struct mq_attr *);
        va_end(ap);

        /* Create exclusively first so exactly one opener initializes it. */
        fd = shm_open(shm, O_RDWR | O_CREAT | O_EXCL, mode);
        if (fd != -1) {
            if (mq_init(fd, attr) == -1) {
                int e = errno;
                (void) close(fd);
                (void) shm_unlink(shm);
                errno = e;
                return (mqd_t) -1;
            }
            return mq_finish(fd, oflag);
        }
        if (errno != EEXIST || (oflag & O_EXCL))
            return (mqd_t) -1; /* real error, or O_EXCL and it exists */
        /* Already exists and O_EXCL not set: fall through to a plain open. */
    }

    fd = shm_open(shm, O_RDWR, 0);
    if (fd == -1)
        return (mqd_t) -1;
    if (mq_wait_ready(fd) == -1) {
        int e = errno;
        (void) close(fd);
        errno = e;
        return (mqd_t) -1;
    }
    return mq_finish(fd, oflag);
}

int mq_close(mqd_t mqdes)
{
    /* Only close descriptors we handed out; a stray fd must not take an
     * unrelated file down with it.
     */
    if (!mq_desc_del((int) mqdes)) {
        errno = EBADF;
        return -1;
    }
    return close((int) mqdes);
}

int mq_unlink(const char *name)
{
    char shm[NAME_MAX + 8];

    if (!mq_name_ok(name)) {
        errno = EINVAL;
        return -1;
    }
    if (mq_shm_name(name, shm, sizeof shm) == -1)
        return -1;
    return shm_unlink(shm);
}

/* Map a queue from its descriptor for one operation. The mapping is per-call
 * (simple, if not the fastest); the shared spin lock lives in the underlying
 * physical memory, so it serializes across separate mappings of the object.
 */
static struct mq_shm *mq_map(int fd, size_t *sizep)
{
    struct stat st;
    struct mq_shm *hdr;

    if (fstat(fd, &st) == -1)
        return NULL;
    if ((size_t) st.st_size < sizeof(struct mq_shm)) {
        errno = EBADF;
        return NULL;
    }
    hdr = mmap(NULL, (size_t) st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED,
               fd, 0);
    if (hdr == MAP_FAILED)
        return NULL;

    /* Trust the mapping only if it is a fully-initialized queue whose ring
     * actually fits — a stray or corrupt fd must not let the slot math walk off
     * the mapping or spin on a garbage lock.
     */
    if (atomic_load_explicit(&hdr->magic, memory_order_acquire) != MQ_MAGIC ||
        hdr->attr.mq_maxmsg <= 0 || hdr->attr.mq_maxmsg > MQ_MAXMSG_LIMIT ||
        hdr->attr.mq_msgsize <= 0 || hdr->attr.mq_msgsize > MQ_MSGSIZE_LIMIT ||
        mq_object_size(hdr->attr.mq_maxmsg, hdr->attr.mq_msgsize) >
            (size_t) st.st_size) {
        (void) munmap(hdr, (size_t) st.st_size);
        errno = EBADF;
        return NULL;
    }
    *sizep = (size_t) st.st_size;
    return hdr;
}

static struct mq_slot *mq_slot_at(struct mq_shm *hdr, long i)
{
    char *base = (char *) hdr + mq_align(sizeof(struct mq_shm));
    return (struct mq_slot *) (base + (size_t) i *
                                          mq_slot_stride(hdr->attr.mq_msgsize));
}

/* True once the absolute CLOCK_REALTIME deadline has passed. A NULL abstime
 * means "block indefinitely", which never times out.
 */
static int mq_timedout(const struct timespec *abstime)
{
    struct timespec now;

    if (!abstime)
        return 0;
    if (clock_gettime(CLOCK_REALTIME, &now) == -1)
        return 0;
    if (now.tv_sec != abstime->tv_sec)
        return now.tv_sec > abstime->tv_sec;
    return now.tv_nsec >= abstime->tv_nsec;
}

/* POSIX only requires the deadline to be validated when the call would block.
 */
static int mq_abstime_bad(const struct timespec *abstime)
{
    return abstime && (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L);
}

/* Decide whether a full (send) or empty (receive) queue should keep blocking.
 * Returns 0 after yielding, so the caller retries; returns -1 with errno set
 * when it should give up: EAGAIN for O_NONBLOCK, EINVAL for a malformed
 * deadline, ETIMEDOUT once the deadline has passed.
 */
static int mq_block_wait(int flags, const struct timespec *abstime)
{
    if (flags & O_NONBLOCK) {
        errno = EAGAIN;
        return -1;
    }
    if (mq_abstime_bad(abstime)) {
        errno = EINVAL;
        return -1;
    }
    if (mq_timedout(abstime)) {
        errno = ETIMEDOUT;
        return -1;
    }
    sched_yield();
    return 0;
}

static int mq_send_core(mqd_t mqdes,
                        const char *msg_ptr,
                        size_t msg_len,
                        unsigned msg_prio,
                        const struct timespec *abstime)
{
    struct mq_shm *hdr;
    size_t size;
    int flags, ret = -1;

    if (msg_prio >= MQ_PRIO_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (msg_len != 0 && !msg_ptr) {
        errno = EFAULT;
        return -1;
    }
    if ((flags = mq_desc_flags((int) mqdes)) == -1)
        return -1;
    if ((flags & O_ACCMODE) == O_RDONLY) {
        errno = EBADF; /* queue was opened read-only */
        return -1;
    }
    if (!(hdr = mq_map((int) mqdes, &size)))
        return -1;
    if (msg_len > (size_t) hdr->attr.mq_msgsize) {
        errno = EMSGSIZE;
        goto out;
    }

    for (;;) {
        pthread_spin_lock(&hdr->lock);
        if (hdr->attr.mq_curmsgs < hdr->attr.mq_maxmsg) {
            for (long i = 0; i < hdr->attr.mq_maxmsg; i++) {
                struct mq_slot *slot = mq_slot_at(hdr, i);

                if (!slot->mq_used) {
                    slot->mq_prio = (long) msg_prio;
                    slot->mq_seq = hdr->seq_next++;
                    slot->mq_len = msg_len;
                    slot->mq_used = 1;
                    if (msg_len) /* a zero-length send has no (maybe NULL) body
                                  */
                        memcpy((char *) slot + sizeof(struct mq_slot), msg_ptr,
                               msg_len);
                    hdr->attr.mq_curmsgs++;
                    break;
                }
            }
            pthread_spin_unlock(&hdr->lock);
            ret = 0;
            goto out;
        }
        pthread_spin_unlock(&hdr->lock);

        /* Queue full: O_NONBLOCK fails fast, a timed call honors its deadline,
         * an untimed call blocks (abstime NULL never times out).
         */
        if (mq_block_wait(flags, abstime) == -1)
            goto out;
    }
out:
    (void) munmap(hdr, size);
    return ret;
}

static ssize_t mq_receive_core(mqd_t mqdes,
                               char *msg_ptr,
                               size_t msg_len,
                               unsigned *msg_prio,
                               const struct timespec *abstime)
{
    struct mq_shm *hdr;
    size_t size;
    int flags;
    ssize_t ret = -1;

    if (msg_len != 0 && !msg_ptr) {
        errno = EFAULT;
        return -1;
    }
    if ((flags = mq_desc_flags((int) mqdes)) == -1)
        return -1;
    if ((flags & O_ACCMODE) == O_WRONLY) {
        errno = EBADF; /* queue was opened write-only */
        return -1;
    }
    if (!(hdr = mq_map((int) mqdes, &size)))
        return -1;
    if (msg_len < (size_t) hdr->attr.mq_msgsize) {
        errno = EMSGSIZE;
        goto out;
    }

    for (;;) {
        pthread_spin_lock(&hdr->lock);
        if (hdr->attr.mq_curmsgs != 0) {
            struct mq_slot *best = NULL;

            for (long i = 0; i < hdr->attr.mq_maxmsg; i++) {
                struct mq_slot *slot = mq_slot_at(hdr, i);

                if (!slot->mq_used)
                    continue;
                /* Highest priority wins; oldest sequence breaks ties (FIFO). */
                if (!best || slot->mq_prio > best->mq_prio ||
                    (slot->mq_prio == best->mq_prio &&
                     slot->mq_seq < best->mq_seq))
                    best = slot;
            }
            memcpy(msg_ptr, (char *) best + sizeof(struct mq_slot),
                   best->mq_len);
            if (msg_prio)
                *msg_prio = (unsigned) best->mq_prio;
            ret = (ssize_t) best->mq_len;
            best->mq_used = 0;
            hdr->attr.mq_curmsgs--;
            pthread_spin_unlock(&hdr->lock);
            goto out;
        }
        pthread_spin_unlock(&hdr->lock);

        /* Queue empty: O_NONBLOCK fails fast, a timed call honors its deadline,
         * an untimed call blocks (abstime NULL never times out).
         */
        if (mq_block_wait(flags, abstime) == -1)
            goto out;
    }
out:
    (void) munmap(hdr, size);
    return ret;
}

int mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len, unsigned msg_prio)
{
    return mq_send_core(mqdes, msg_ptr, msg_len, msg_prio, NULL);
}

int mq_timedsend(mqd_t mqdes,
                 const char *msg_ptr,
                 size_t msg_len,
                 unsigned msg_prio,
                 const struct timespec *abstime)
{
    return mq_send_core(mqdes, msg_ptr, msg_len, msg_prio, abstime);
}

ssize_t mq_receive(mqd_t mqdes,
                   char *msg_ptr,
                   size_t msg_len,
                   unsigned *msg_prio)
{
    return mq_receive_core(mqdes, msg_ptr, msg_len, msg_prio, NULL);
}

ssize_t mq_timedreceive(mqd_t mqdes,
                        char *msg_ptr,
                        size_t msg_len,
                        unsigned *msg_prio,
                        const struct timespec *abstime)
{
    return mq_receive_core(mqdes, msg_ptr, msg_len, msg_prio, abstime);
}

int mq_getattr(mqd_t mqdes, struct mq_attr *attr)
{
    struct mq_shm *hdr;
    size_t size;
    int flags;

    if (!attr) {
        errno = EINVAL;
        return -1;
    }
    if ((flags = mq_desc_flags((int) mqdes)) == -1)
        return -1;
    if (!(hdr = mq_map((int) mqdes, &size)))
        return -1;
    pthread_spin_lock(&hdr->lock);
    *attr = hdr->attr;
    pthread_spin_unlock(&hdr->lock);
    attr->mq_flags = flags & O_NONBLOCK; /* per-descriptor, not shared */
    (void) munmap(hdr, size);
    return 0;
}

int mq_setattr(mqd_t mqdes, const struct mq_attr *attr, struct mq_attr *oattr)
{
    struct mq_shm *hdr;
    size_t size;
    int old;

    if (!attr) {
        errno = EINVAL;
        return -1;
    }
    /* Validate the descriptor and read its current flags without mutating. */
    if ((old = mq_desc_flags((int) mqdes)) == -1)
        return -1;
    if (oattr) {
        if (!(hdr = mq_map((int) mqdes, &size)))
            return -1;
        pthread_spin_lock(&hdr->lock);
        *oattr = hdr->attr;
        pthread_spin_unlock(&hdr->lock);
        oattr->mq_flags = old & O_NONBLOCK;
        (void) munmap(hdr, size);
    }

    /* POSIX: only mq_flags (O_NONBLOCK) is settable, and it belongs to the
     * descriptor. Done last, so a mapping failure above leaves it untouched.
     */
    (void) mq_desc_setblock((int) mqdes, (int) (attr->mq_flags & O_NONBLOCK));
    return 0;
}
