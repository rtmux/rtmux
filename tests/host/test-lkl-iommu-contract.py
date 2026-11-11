#!/usr/bin/env python3
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
"""Q86/Q87 regression: LKL virtio-net IOMMU handoff and virtio 3.1 ordering.

The real source of virtio_mmio_require_iommu_domain() (servers/lkl/virtio.c) and
of virtnet_init() with its callees (servers/lkl/host.c) is extracted
and compiled on the host against recording stubs for MMIO, queues and the
domain probe.  Three paths are pinned:

  no-stream       firmware described no IOMMU stream: the device is trusted
                  without a DMA domain (probe never runs), RX buffers are
                  posted and DRIVER_OK is written.
  described+ENOTSUP  stream described, probe succeeds, but enforcement is
                  unavailable: init fails ENOTSUP before any RX descriptor is
                  posted and before DRIVER_OK.
  described+probe-fail  probe error: also refused before RX/DRIVER_OK.

The first case documents the CURRENT trusted-device policy; it is not a claim
that the device is isolated.  Changing that policy must update this test.
Q87: the real virtio_queue_kick() (M, w,o fence, QUEUE_NOTIFY for its own qid, RX
and TX) runs un-stubbed inside this harness; only its barrier/MMIO callees record.
Q87: the real virtnet_stage_locked/publish/rx_stage/rx_post are extracted too
and run against real descriptor/avail arrays with an ordered event log of
status writes, barriers and kicks (v1 and v2).  Init must stage all RX
buffers with no kick, fence, write DRIVER_OK, then kick RX exactly once;
steady-state RX repost and TX publish must still notify.
Mutations of the extracted source must be caught, so the harness is not vacuous.
Queue allocation/publication: the real virtio_queue_alloc()/virtio_queue_publish()
are compiled too, against a recording MMIO log and fake DMA memory, for both
virtio v1 and v2.  Checked:
no queue address/QUEUE_PFN/QUEUE_READY write before the domain decision (probe
time and refusal); published registers equal the allocated addresses and lie in
the ranges handed to the probe; the other version's registers stay zero; v1
PFN overflow is refused without leaks; and a modelled memory-to-I/O fence
(virtio_fence_w_o) precedes PFN/READY/DRIVER_OK after any ring store.
Queue-control I/O ordering is modelled with an event log of MMIO accesses and
fences (pred/succ sets).  Required, each by one fence of sufficient scope:
any earlier MMIO -> QUEUE_SEL write (io,o: covers the previous queue's
NUM_MAX read/READY, i.e. cross-queue publication); QUEUE_SEL -> NUM_MAX read
(o,i); QUEUE_SEL -> NUM, config -> PFN/READY, and PFN/READY -> any later MMIO
write incl. DRIVER_OK (o,o).  Config-to-config writes are deliberately not
required to be ordered.  The Q87 init order composes with this: queues are
published (trailing o,o) after the domain decision, RX buffers are staged with
no kick, fence w,o precedes DRIVER_OK, fence o,o follows it, then one RX kick.
Modes: --host (default; needs no RISC-V compiler) runs everything above;
--riscv-asm (make host-lkl-fence-asm RISCV_CC=...) runs only the cross-assembly
fence check of all four helpers (w,o / o,o / io,o / o,i) and exits non-zero if
RISCV_CC is unset or fails.  Host-only: no QEMU.
Limits: the harness only proves call order and values against stubs, and the
log model is a checker of the intended fences, not of RVWMO or any platform's
I/O region attributes.  It does NOT show: posted-write completion (no read-back
or completion barrier is added), IRQ ACK/ordering, device reset, status
read-back/FAILED handling, that a device honors the ranges, steady-state kick
I/O ordering, or any hardware isolation.  MMIO accessors stay unfenced.  MMIO,
discovery and the domain probe are stubbed; the avail ring is zero-filled by the
fake DMA allocator, so "ring[0] == 0" is not a discriminating check.
"""
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CC = shlex.split(os.environ.get("HOST_CC", "cc"))
if not CC:
    sys.exit("HOST_CC must name a host compiler")


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def extract(src, name):
    m = re.search(r"^[A-Za-z_][\w \*]*\b%s\(" % re.escape(name), src, re.M)
    if not m:
        sys.exit("cannot find function %s" % name)
    i = src.index("\n{", m.start()) + 1
    if ";" in src[m.start():i]:
        sys.exit("declaration found before definition of %s" % name)
    depth, j = 0, i
    while True:
        c = src[j]
        if c == '"':
            j += 1
            while src[j] != '"':
                j += 2 if src[j] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[m.start():j + 1] + "\n"
        j += 1


def extract_struct(src, head):
    i = src.index(head)
    return src[i:src.index("\n};", i) + 3] + "\n"


def defines(src, prefix):
    return "".join(l + "\n" for l in src.splitlines() if l.startswith("#define " + prefix))


def mutate(text, old, new):
    if text.count(old) != 1:
        sys.exit("mutation anchor not unique/present: %r" % old)
    return text.replace(old, new)


virtio_c = read("servers/lkl/virtio.c")
host_c = read("servers/lkl/host.c")
msg_h = read("internal/include/sys/virtio-mmio-msg.h")

REQ = extract(virtio_c, "virtio_mmio_require_iommu_domain")
ADD = extract(host_c, "virtnet_add_dma_range")
HREQ = extract(host_c, "virtnet_require_iommu_domain")
INIT = extract(host_c, "virtnet_init")
DISC = extract(host_c, "virtnet_discover")
STAGE = extract(host_c, "virtnet_stage_locked")
PUBLISH = extract(host_c, "virtnet_publish_locked")
RXSTAGE = extract(host_c, "virtnet_rx_stage_locked")
RXPOST = extract(host_c, "virtnet_rx_post_locked")
QALLOC = extract(virtio_c, "virtio_queue_alloc")
QPUB = extract(virtio_c, "virtio_queue_publish")
KICK = extract(virtio_c, "virtio_queue_kick")
QDEFS = defines(virtio_c, "VIRTIO_MMIO_QUEUE_") + defines(virtio_c, "VIRTIO_DMA_")
NETDEV = extract_struct(host_c, "struct rtmux_lkl_netdev {")
# Single-line stream macros only; the DECODE macro continues on the next line.
STREAM = re.search(r"#define RTMUX_RESOURCE_IOMMU_STREAM_NONE[^\n]*\n", msg_h).group(0) + \
    re.search(r"#define RTMUX_RESOURCE_IOMMU_STREAM_ENCODE[^\n]*\n", msg_h).group(0) + \
    re.search(r"#define RTMUX_RESOURCE_IOMMU_STREAM_DECODE\(.*?\n[^\n]*\n", msg_h, re.S).group(0)
CONSTS = defines(host_c, "VIRTNET_")

HARNESS = r'''
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "virtio.h"
#define EOK 0
%(STREAM)s
%(CONSTS)s
%(QDEFS)s
#include <align.h>
struct lkl_netdev { int dummy; };
%(NETDEV)s

static int probe_rc, probe_calls, driver_ok, status_writes;
static int dev_version = 2, dma_fail_after, dma_n, dma_allocs, dma_frees;
static int qwrites, qwrites_at_probe, ready_cnt, pfn_cnt, sel, num_max_reads;
static int ring_dirty, bad_fence;
static uint64_t range_pa[32];
static size_t range_sz[32];
static unsigned range_n;
static uint32_t reg[2][0x80];       /* per-selected-queue register file */
static uint64_t next_pa = 0x200000;
static struct rtmux_lkl_netdev nd;
#define rxq_seen (nd.rxq)
#define txq_seen (nd.txq)
/* Q87 ordered event log: 'S' status write, 'K' kick (qid in arg), 'M' full
 * barrier, 'W' fence w,o, 'O' fence o,o. */
struct ev { char t; uint32_t arg; uint16_t avail_idx; uint64_t desc_addr; uint32_t desc_len; uint16_t desc_flags, desc_next; };
static struct ev evs[512];
static int nev, event_overflow;
static uint64_t rx_paddr[VIRTNET_Q_SIZE];
static void ev(char t, uint32_t arg, uint16_t idx)
{ if (nev < 512) evs[nev++] = (struct ev) { .t = t, .arg = arg, .avail_idx = idx };
  else event_overflow = 1; }
static struct rtmux_virtio_queue *watch_q;
void virtio_mb(void)
{
    ev('M', watch_q ? watch_q->avail->ring[watch_q->avail->idx %% watch_q->num] : 0,
       rxq_seen ? rxq_seen->avail->idx : 0);
    if (watch_q && !event_overflow) {
        struct ev *last = &evs[nev - 1];
        last->avail_idx = watch_q->avail->idx;
        if (last->arg < watch_q->num) {
            last->desc_addr = watch_q->desc[last->arg].addr;
            last->desc_len = watch_q->desc[last->arg].len;
            last->desc_flags = watch_q->desc[last->arg].flags;
            last->desc_next = watch_q->desc[last->arg].next;
        }
    }
}
static int virtio_mmio_probe_iommu_domain(struct rtmux_virtio_mmio *vio,
    const struct rtmux_virtio_dma_range *r, unsigned c, size_t b)
{
    (void) vio; (void) b; probe_calls++; qwrites_at_probe = qwrites;
    range_n = c;
    for (unsigned i = 0; i < c && i < 32; i++) { range_pa[i] = r[i].paddr; range_sz[i] = r[i].size; }
    return probe_rc;
}

/* event log: 'R'/'W' MMIO access, 'F' fence with pred/succ masks */
enum { FI = 1, FO = 2, FR = 4, FW = 8 };
static struct { char k; unsigned off; unsigned pred, succ; uint32_t v; } mio[256];
static int nmio, bad_order, mio_overflow;
static void logev(char k, unsigned off, unsigned p, unsigned sc, uint32_t v)
{ if (nmio < 256) { mio[nmio].k = k; mio[nmio].off = off; mio[nmio].pred = p; mio[nmio].succ = sc; mio[nmio].v = v; nmio++; }
  else mio_overflow = 1; }
static int is_cfg(unsigned o)
{ return o == VIRTIO_MMIO_QUEUE_NUM || o == VIRTIO_MMIO_QUEUE_ALIGN ||
         (o >= VIRTIO_MMIO_QUEUE_DESC_LOW && o <= VIRTIO_MMIO_QUEUE_DEVICE_HIGH); }
static int is_live(unsigned o)
{ return o == VIRTIO_MMIO_QUEUE_PFN || o == VIRTIO_MMIO_QUEUE_READY; }
/* Is there a single fence between events a and b with pred>=type(a), succ>=type(b)? */
static int fenced(int a, int b)
{
    unsigned pa = mio[a].k == 'R' ? FI : FO, sb = mio[b].k == 'R' ? FI : FO;
    for (int i = a + 1; i < b; i++)
        if (mio[i].k == 'F' && (mio[i].pred & pa) == pa && (mio[i].succ & sb) == sb) return 1;
    return 0;
}
/* Does an ordering rule require a fence between earlier access a and later access b? */
static int need_fence(int a, int b)
{
    unsigned oa = mio[a].off, ob = mio[b].off;
    int wa = mio[a].k == 'W', wb = mio[b].k == 'W';
    if (wb && ob == VIRTIO_MMIO_QUEUE_SEL) return 1;                  /* any -> SEL */
    if (wa && oa == VIRTIO_MMIO_QUEUE_SEL && !wb && ob == VIRTIO_MMIO_QUEUE_NUM_MAX) return 1;
    if (wa && oa == VIRTIO_MMIO_QUEUE_SEL && wb && ob == VIRTIO_MMIO_QUEUE_NUM) return 1;
    if (wa && is_cfg(oa) && wb && is_live(ob)) return 1;             /* config -> PFN/READY */
    if (wa && is_live(oa)) return 1;                                  /* live -> anything */
    if (wa && wb && ob == VIRTIO_MMIO_STATUS && (mio[b].v & VIRTIO_STATUS_DRIVER_OK))
        return 1;                                                     /* any write -> DRIVER_OK */
    return 0;
}
/* Check the queue-control ordering rules over the log of the last run.  Every
 * earlier access is a candidate predecessor, not just the adjacent one: an
 * intervening read must not hide a missing fence between two writes.
 */
static void check_io_order(void)
{
    if (mio_overflow) bad_order++;       /* a truncated log proves nothing */
    for (int j = 0; j < nmio; j++) {
        if (mio[j].k == 'F') continue;
        for (int i = 0; i < j; i++)
            if (mio[i].k != 'F' && need_fence(i, j) && !fenced(i, j)) bad_order++;
    }
}
void virtio_io_fence_before_sel(void) { logev('F', 0, FI | FO, FO, 0); }
void virtio_fence_o_o(void)
{ ev('O', 0, rxq_seen ? rxq_seen->avail->idx : 0); logev('F', 0, FO, FO, 0); }
void virtio_io_fence_oi(void) { logev('F', 0, FO, FI, 0); }

uint32_t virtio_mmio_rd32(struct rtmux_virtio_mmio *vio, unsigned off)
{
    (void) vio;
    logev('R', off, 0, 0, 0);
    if (off == VIRTIO_MMIO_QUEUE_NUM_MAX) { num_max_reads++; return 256; }
    return 0xffffffffU; /* FEATURES_OK/VERSION_1 set */
}
void virtio_fence_w_o(void)
{ ev('W', 0, rxq_seen ? rxq_seen->avail->idx : 0); ring_dirty = 0; logev('F', 0, FW, FO, 0); }
void virtio_mmio_wr32(struct rtmux_virtio_mmio *vio, unsigned off, uint32_t v)
{
    (void) vio;
    logev('W', off, 0, 0, v);
    if (off == VIRTIO_MMIO_QUEUE_NOTIFY) {
        /* the real virtio_queue_kick() lands here; record the notified queue */
        struct rtmux_virtio_queue *kq = v == VIRTNET_RXQ ? nd.rxq : nd.txq;
        ev('K', v, kq ? kq->avail->idx : 0);
        return;
    }
    if (off == VIRTIO_MMIO_STATUS) {
        status_writes++;
        ev('S', v, rxq_seen ? rxq_seen->avail->idx : 0);
        if (v & VIRTIO_STATUS_DRIVER_OK) driver_ok++;
    }
    if (off == VIRTIO_MMIO_QUEUE_SEL) { sel = (int) v; return; }
    if (off == VIRTIO_MMIO_QUEUE_NUM || off == VIRTIO_MMIO_QUEUE_ALIGN ||
        off == VIRTIO_MMIO_QUEUE_PFN || off == VIRTIO_MMIO_QUEUE_READY ||
        (off >= VIRTIO_MMIO_QUEUE_DESC_LOW && off <= VIRTIO_MMIO_QUEUE_DEVICE_HIGH)) {
        qwrites++;
        reg[sel & 1][off / 4] = v;
        if (off == VIRTIO_MMIO_QUEUE_READY) ready_cnt++;
        if (off == VIRTIO_MMIO_QUEUE_PFN) pfn_cnt++;
        if ((off == VIRTIO_MMIO_QUEUE_READY || off == VIRTIO_MMIO_QUEUE_PFN) && ring_dirty)
            bad_fence++;
    }
}
int virtio_mmio_discover(struct rtmux_virtio_mmio *v, uint32_t id, int *ver)
{ (void) v; (void) id; *ver = dev_version; return 0; }

static void *virtio_dma_alloc(size_t size, uint64_t *pa)
{
    void *p;
    if (dma_fail_after && ++dma_n >= dma_fail_after) return NULL;
    p = calloc(1, size);
    *pa = next_pa; next_pa += (size + 0xfffU) & ~0xfffULL;
    dma_allocs++; ring_dirty = 1;
    return p;
}
static void virtio_dma_free(void *p, size_t size)
{ (void) size; if (p) dma_frees++; free(p); }
int virtio_dma_pool_alloc(struct rtmux_virtio_dma_pool *p, unsigned n, size_t sz)
{
    static uint64_t next = 0x100000;
    memset(p, 0, sizeof *p); p->count = n; p->size = sz;
    for (unsigned i = 0; i < n; i++) { p->buf[i].paddr = next; next += 0x1000; }
    return 0;
}
int virtio_irq_start(struct rtmux_virtio_mmio *v, void (*f)(void *), void *a)
{ (void) v; (void) f; (void) a; return 0; }

static void virtnet_irq_notify(void *arg) { (void) arg; }
%(QALLOC)s
%(QPUB)s
%(KICK)s
%(DISC)s
%(REQ)s
%(ADD)s
%(STAGE)s
%(PUBLISH)s
%(RXSTAGE)s
%(RXPOST)s
%(HREQ)s
%(INIT)s

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %%s: %%s\n", __func__, #c); fails++; } } while (0)

static int count_kicks(void)
{ int n = 0; for (int i = 0; i < nev; i++) if (evs[i].t == 'K') n++; return n; }
/* The real virtio_queue_kick() must be a full barrier, then fence w,o, then the
 * QUEUE_NOTIFY store for the queue's own qid: M, W, K back to back. */
static void check_kicks(void)
{
    for (int i = 0; i < nev; i++) {
        if (evs[i].t != 'K') continue;
        CHECK(i >= 2 && evs[i - 1].t == 'W');
        if (i >= 2 && evs[i - 1].t == 'W')   /* fence present: full barrier must precede it */
            CHECK(evs[i - 2].t == 'M');
    }
}
static int rx_staged(void) { return rxq_seen ? rxq_seen->avail->idx : 0; }

static void run(uint32_t stream, int prc, int *rc_out)
{
    memset(&nd, 0, sizeof nd);
    nd.vio.iommu_stream = stream;
    nd.vio.iommu_phandle = stream ? 7 : 0;
    probe_rc = prc; probe_calls = driver_ok = status_writes = 0;
    qwrites = qwrites_at_probe = ready_cnt = pfn_cnt = num_max_reads = 0;
    dma_n = dma_allocs = dma_frees = range_n = 0;
    ring_dirty = bad_fence = 0; sel = 0; nmio = bad_order = mio_overflow = 0;
    nev = event_overflow = 0;
    memset(reg, 0, sizeof reg);
    *rc_out = virtnet_init(&nd);
    for (unsigned i = 0; i < VIRTNET_Q_SIZE; i++) rx_paddr[i] = nd.rx_pool.buf[i].paddr;
}

/* Init event order: every RX descriptor is staged with no kick, a barrier
 * precedes DRIVER_OK, exactly one RX kick follows DRIVER_OK, and TX is never
 * kicked during init. The avail idx seen at DRIVER_OK and at the kick must be
 * the fully staged ring, with each descriptor/ring slot filled. */
static void check_order(int version)
{
    int dok = -1, kick = -1, mb_before = 0, oo_after = 0;

    for (int i = 0; i < nev; i++) {
        if (evs[i].t == 'S' && (evs[i].arg & VIRTIO_STATUS_DRIVER_OK)) {
            dok = i;
            /* w,o fence directly before DRIVER_OK, after the last idx bump */
            mb_before = i > 0 && evs[i - 1].t == 'W' && evs[i - 1].avail_idx == VIRTNET_Q_SIZE;
            /* o,o fence directly after DRIVER_OK, before any notification */
            oo_after = i + 1 < nev && evs[i + 1].t == 'O';
            CHECK(evs[i].avail_idx == VIRTNET_Q_SIZE);
            CHECK(version == 2 ? (evs[i].arg & VIRTIO_STATUS_FEATURES_OK) != 0
                               : (evs[i].arg & VIRTIO_STATUS_FEATURES_OK) == 0);
        }
        if (evs[i].t == 'K') {
            CHECK(evs[i].arg == VIRTNET_RXQ);   /* never TX during init */
            CHECK(dok >= 0 && i > dok);          /* strictly after DRIVER_OK */
            if (kick < 0) kick = i;
        }
    }
    CHECK(!event_overflow);
    check_kicks();
    CHECK(dok >= 0);
    CHECK(mb_before);
    CHECK(oo_after);
    CHECK(count_kicks() == 1);
    CHECK(kick > dok);
    CHECK(kick >= 0 && evs[kick].avail_idx == VIRTNET_Q_SIZE);
    CHECK(nev > 0 && evs[nev - 1].t == 'K');                /* kick is the last init event */
    for (unsigned i = 0; i < VIRTNET_Q_SIZE; i++) {
        CHECK(rxq_seen->avail->ring[i] == i);
        CHECK(rxq_seen->desc[i].flags == VRING_DESC_F_WRITE);
        CHECK(rxq_seen->desc[i].len == VIRTNET_BUF_SIZE);
        CHECK(rxq_seen->desc[i].addr == rx_paddr[i]);
    }
    CHECK(txq_seen->avail->idx == 0);
}

/* Normal operation after DRIVER_OK: RX repost and TX publish both notify. */
static void steady_state(void)
{
    int rc;

    run(RTMUX_RESOURCE_IOMMU_STREAM_NONE, EOK, &rc);
    nd.vio = (struct rtmux_virtio_mmio) { 0 };
    nd.rx_pool.buf[3].paddr = 0xabc000;
    nev = 0;
    rxq_seen->avail->ring[0] = UINT16_MAX;
    virtnet_rx_post_locked(&nd, 3);
    CHECK(count_kicks() == 1);
    CHECK(nev > 0 && evs[nev - 1].t == 'K' && nev > 0 && evs[nev - 1].arg == VIRTNET_RXQ);
    CHECK(nev > 0 && evs[nev - 1].avail_idx == VIRTNET_Q_SIZE + 1);
    check_kicks();
    CHECK(rxq_seen->desc[3].addr == 0xabc000);
    CHECK(rxq_seen->desc[3].flags == VRING_DESC_F_WRITE);
    CHECK(rxq_seen->desc[3].len == VIRTNET_BUF_SIZE);
    CHECK(rxq_seen->desc[3].next == 0);
    CHECK(rxq_seen->avail->ring[0] == 3);
    nev = 0;
    for (unsigned i = 0; i < txq_seen->num; i++) txq_seen->avail->ring[i] = 0xffff;
    watch_q = txq_seen;
    txq_seen->desc[5].next = UINT16_MAX;
    virtnet_publish_locked(&nd.vio, txq_seen, 5, 0x5000, 64, VRING_DESC_F_NEXT);
    CHECK(count_kicks() == 1);
    CHECK(nev > 0 && evs[nev - 1].arg == VIRTNET_TXQ && evs[nev - 1].avail_idx == 1);
    check_kicks();
    CHECK(txq_seen->desc[5].len == 64);
    /* at the first barrier the ring slot is filled and idx not yet bumped */
    CHECK(evs[0].t == 'M' && evs[0].arg == 5 && evs[0].avail_idx == 0);
    CHECK(evs[0].t != 'M' || evs[0].arg != 5 || (evs[0].desc_addr == 0x5000 && evs[0].desc_len == 64));
    CHECK(evs[0].t != 'M' || evs[0].arg != 5 || (evs[0].desc_flags == VRING_DESC_F_NEXT && evs[0].desc_next == 0));
    CHECK(!event_overflow);
    watch_q = NULL;
    /* barrier between ring-slot write and idx bump precedes the kick */
    CHECK(nev >= 3);
}

static int in_ranges(uint64_t pa, size_t size)
{
    for (unsigned i = 0; i < range_n; i++)
        if (range_pa[i] == pa && range_sz[i] == size) return 1;
    return 0;
}

static uint64_t reg64(unsigned q, unsigned lo)
{ return ((uint64_t) reg[q][(lo + 4) / 4] << 32) | reg[q][lo / 4]; }

/* Published registers must be exactly the allocations, in the probed ranges,
 * and the registers of the other virtio version must stay zero.
 */
static void check_published(int version)
{
    struct rtmux_virtio_queue *qs[2] = { nd.rxq, nd.txq };
    CHECK(qs[0] && qs[1] && qs[0] != qs[1]);
    /* ranges as the real code would build them for this netdev */
    range_n = 0;
    nd.vio.iommu_stream = RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(42);
    (void) virtnet_require_iommu_domain(&nd);
    for (unsigned i = 0; i < 2 && qs[i]; i++) {
        struct rtmux_virtio_queue *q = qs[i];
        CHECK(reg[i][VIRTIO_MMIO_QUEUE_NUM / 4] == q->num);
        CHECK(q->num == VIRTNET_Q_SIZE);
        if (version == 1) {
            CHECK(reg[i][VIRTIO_MMIO_QUEUE_ALIGN / 4] == 4096);
            CHECK(((uint64_t) reg[i][VIRTIO_MMIO_QUEUE_PFN / 4] << 12) == q->mem_pa);
            CHECK(in_ranges(q->mem_pa, q->mem_size));
            CHECK(reg[i][VIRTIO_MMIO_QUEUE_READY / 4] == 0);
            CHECK(reg64(i, VIRTIO_MMIO_QUEUE_DESC_LOW) == 0);
            CHECK(reg64(i, VIRTIO_MMIO_QUEUE_DRIVER_LOW) == 0);
            CHECK(reg64(i, VIRTIO_MMIO_QUEUE_DEVICE_LOW) == 0);
        } else {
            CHECK(reg64(i, VIRTIO_MMIO_QUEUE_DESC_LOW) == q->desc_pa);
            CHECK(reg64(i, VIRTIO_MMIO_QUEUE_DRIVER_LOW) == q->avail_pa);
            CHECK(reg64(i, VIRTIO_MMIO_QUEUE_DEVICE_LOW) == q->used_pa);
            CHECK(in_ranges(q->desc_pa, 4096) && in_ranges(q->avail_pa, 4096) &&
                  in_ranges(q->used_pa, 4096));
            CHECK(reg[i][VIRTIO_MMIO_QUEUE_READY / 4] == 1);
            CHECK(reg[i][VIRTIO_MMIO_QUEUE_ALIGN / 4] == 0);
            CHECK(reg[i][VIRTIO_MMIO_QUEUE_PFN / 4] == 0);
        }
    }
    CHECK(reg64(0, VIRTIO_MMIO_QUEUE_DESC_LOW) != reg64(1, VIRTIO_MMIO_QUEUE_DESC_LOW) ||
          version == 1);
}

int main(void)
{
    int rc;
    (void) virtnet_rx_stage_locked; /* mutations may orphan it */
    struct rtmux_virtio_dma_range one = { 0x1000, 4096 };
    struct rtmux_virtio_mmio vio;

    /* direct helper contract */
    memset(&vio, 0, sizeof vio);
    probe_calls = 0; probe_rc = EOK;
    CHECK(virtio_mmio_require_iommu_domain(&vio, &one, 1) == EOK);
    CHECK(probe_calls == 0);
    vio.iommu_stream = RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(42);
    CHECK(virtio_mmio_require_iommu_domain(&vio, &one, 1) == ENOTSUP);
    CHECK(probe_calls == 1);
    probe_rc = ENODEV;
    CHECK(virtio_mmio_require_iommu_domain(&vio, &one, 1) == ENODEV);
    vio.iommu_stream = RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(0); /* stream id 0 is described */
    probe_rc = EOK; probe_calls = 0;
    CHECK(virtio_mmio_require_iommu_domain(&vio, &one, 1) == ENOTSUP);
    CHECK(probe_calls == 1);

    /* full virtnet_init paths */
    for (dev_version = 1; dev_version <= 2; dev_version++) {
        /* trusted (no stream) device: queues published, RX staged, DRIVER_OK, one kick */
        run(RTMUX_RESOURCE_IOMMU_STREAM_NONE, EOK, &rc);
        CHECK(rc == 0);
        CHECK(probe_calls == 0);          /* trusted device: no domain established */
        CHECK(rx_staged() == (int) VIRTNET_Q_SIZE);
        CHECK(driver_ok == 1);
        check_order(dev_version);         /* Q87: stage, w,o, DRIVER_OK, o,o, one kick */
        CHECK(num_max_reads == 2);
        CHECK(dev_version == 2 ? ready_cnt == 2 : pfn_cnt == 2);
        CHECK(bad_fence == 0);            /* w,o precedes PFN/READY after any ring store */
        check_io_order();
        CHECK(bad_order == 0);            /* queue-control o,o / o,i / io,o ordering */
        CHECK(!mio_overflow);
        check_published(dev_version);

        /* described stream + domain refusal: nothing programmed, no RX/DRIVER_OK */
        run(RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(42), EOK, &rc);
        CHECK(rc == -ENOTSUP);
        CHECK(probe_calls == 1);
        CHECK(qwrites_at_probe == 0);     /* device untouched while authorizing */
        CHECK(qwrites == 0);              /* and still untouched after refusal */
        CHECK(ready_cnt == 0 && pfn_cnt == 0);
        CHECK(num_max_reads == 2);        /* NUM_MAX/QUEUE_SEL probing still happens */
        CHECK(rx_staged() == 0);
        CHECK(driver_ok == 0);
        CHECK(count_kicks() == 0);
        CHECK(range_n >= (dev_version == 2 ? 6U : 2U));
        CHECK(in_ranges(nd.rxq->mem_pa, nd.rxq->mem_size) || dev_version == 2);
        CHECK(in_ranges(nd.rxq->desc_pa, 4096) || dev_version == 1);

        run(RTMUX_RESOURCE_IOMMU_STREAM_ENCODE(42), ENODEV, &rc);
        CHECK(rc == -ENODEV);
        CHECK(probe_calls == 1);
        CHECK(qwrites == 0);
        CHECK(rx_staged() == 0);
        CHECK(driver_ok == 0);
        CHECK(count_kicks() == 0);

        /* allocation failure of the second queue: nothing published */
        dma_fail_after = dev_version == 2 ? 4 : 2;
        run(RTMUX_RESOURCE_IOMMU_STREAM_NONE, EOK, &rc);
        dma_fail_after = 0;
        CHECK(rc == -ENOMEM);
        CHECK(qwrites == 0);
        CHECK(probe_calls == 0);
        CHECK(driver_ok == 0);
    }
    dev_version = 2;

    steady_state();

    /* v1 PFN is 32-bit: a ring starting at 2^44 is refused and freed */
    {
        struct rtmux_virtio_queue *q;
        uint64_t save = next_pa;

        memset(&vio, 0, sizeof vio);
        qwrites = dma_allocs = dma_frees = 0;
        next_pa = 1ULL << 44;
        q = virtio_queue_alloc(&vio, 1, 0, VIRTNET_Q_SIZE);
        CHECK(q == NULL);
        CHECK(dma_allocs == 1 && dma_frees == 1);
        CHECK(qwrites == 0);
        next_pa = (1ULL << 44) - 0x1000; /* highest page-aligned PFN still fits */
        q = virtio_queue_alloc(&vio, 1, 0, VIRTNET_Q_SIZE);
        CHECK(q != NULL);
        nmio = bad_order = mio_overflow = 0;
        if (q) { virtio_queue_publish(&vio, 1, q); check_io_order(); CHECK(bad_order == 0); CHECK(reg[0][VIRTIO_MMIO_QUEUE_PFN / 4] ==
                                                        (uint32_t) (q->mem_pa >> 12)); }
        next_pa = save;
    }

    if (!fails) puts("lkl-iommu-contract: ok");
    return fails ? 1 : 0;
}
'''


def build_and_run(tmp, tag, sources, expected_failure=None):
    text = HARNESS % dict(STREAM=STREAM, CONSTS=CONSTS, NETDEV=NETDEV,
                          ADD=ADD, DISC=DISC, HREQ=HREQ, QDEFS=QDEFS, **sources)
    c = os.path.join(tmp, tag + ".c")
    exe = os.path.join(tmp, tag)
    with open(c, "w") as f:
        f.write(text)
    cp = subprocess.run(CC + ["-std=gnu11", "-O0", "-Wall", "-Wextra", "-Werror", "-I" + os.path.join(ROOT, "servers/lkl"),
                         "-I" + os.path.join(ROOT, "servers/lkl/include"), "-I" + os.path.join(ROOT, "lib/common"), "-D_GNU_SOURCE",
                         "-o", exe, c, "-pthread"], capture_output=True, text=True)
    if cp.returncode:
        sys.exit("%s: compile failed\n%s" % (tag, cp.stderr))
    rp = subprocess.run([exe], capture_output=True, text=True)
    if expected_failure is None:
        if rp.returncode != 0:
            sys.exit("%s: baseline failed (exit %s)\n%s\n%s" %
                     (tag, rp.returncode, rp.stdout, rp.stderr))
    else:
        got = {l[5:] for l in rp.stdout.splitlines() if l.startswith("FAIL ")}
        exact = isinstance(expected_failure, (set, frozenset))
        want = set(expected_failure) if exact else {expected_failure}
        bad = got != want if exact else not (want <= got)
        if rp.returncode != 1 or bad:
            sys.exit("%s: mutation failed at the wrong checks (exit %s)\n  want %s%s\n"
                     "  got  %s\n%s" % (tag, rp.returncode, sorted(want),
                                       " (exactly)" if exact else "", sorted(got), rp.stderr))
    print("  %-34s %s" % (tag, "pass" if expected_failure is None else "caught"))


MUTATIONS = [
    ("mut-enforcement-unavailable-ok", "req",
     "    return ENOTSUP;\n}", "    return EOK;\n}"),
    ("mut-described-stream-trusted", "req",
     "    if (vio->iommu_stream == RTMUX_RESOURCE_IOMMU_STREAM_NONE)\n        return EOK;\n",
     "    if (vio->iommu_stream != RTMUX_RESOURCE_IOMMU_STREAM_NONE)\n        return EOK;\n"),
    ("mut-undescribed-refused", "req",
     "    if (vio->iommu_stream == RTMUX_RESOURCE_IOMMU_STREAM_NONE)\n        return EOK;",
     "    if (vio->iommu_stream == RTMUX_RESOURCE_IOMMU_STREAM_NONE)\n        return ENOTSUP;"),
    ("mut-init-ignores-refusal", "init",
     "    if (iommu_rc != EOK)\n        return -iommu_rc;\n", "    (void) iommu_rc;\n"),
    ("mut-rx-before-check", "init",
     "    int iommu_rc = virtnet_require_iommu_domain(rtnd);",
     "    virtnet_rx_post_locked(rtnd, 0);\n    int iommu_rc = virtnet_require_iommu_domain(rtnd);"),
    ("mut-driver-ok-before-check", "init",
     "    int iommu_rc = virtnet_require_iommu_domain(rtnd);",
     "    virtio_mmio_wr32(&rtnd->vio, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_DRIVER_OK);\n"
     "    int iommu_rc = virtnet_require_iommu_domain(rtnd);"),
    # Q87: Virtio 1.2 section 3.1 notification ordering.
    ("mut-init-kick-per-buffer", "init",
     "virtnet_rx_stage_locked(rtnd, i); /* no kick before DRIVER_OK */",
     "virtnet_rx_post_locked(rtnd, i);"),
    ("mut-init-kick-before-driver-ok", "init",
     "    virtio_fence_w_o(); /* staged descriptors and avail->idx before DRIVER_OK */\n",
     "    virtio_queue_kick(&rtnd->vio, rtnd->rxq);\n"),
    ("mut-init-no-kick", "init",
     "    virtio_queue_kick(&rtnd->vio, rtnd->rxq);\n    return 0;", "    return 0;"),
    ("mut-init-kick-tx", "init",
     "    virtio_queue_kick(&rtnd->vio, rtnd->rxq);\n    return 0;",
     "    virtio_queue_kick(&rtnd->vio, rtnd->txq);\n    return 0;"),
    ("mut-init-no-barrier", "init",
     "    virtio_fence_w_o(); /* staged descriptors and avail->idx before DRIVER_OK */\n", ""),
    ("mut-init-full-barrier-not-wo", "init",
     "    virtio_fence_w_o(); /* staged", "    virtio_mb(); /* staged"),
    ("mut-init-no-oo-fence", "init",
     "    virtio_fence_o_o(); /* DRIVER_OK write before the first QUEUE_NOTIFY */\n", ""),
    ("mut-init-oo-before-status", "init",
     "    virtio_fence_w_o(); /* staged descriptors and avail->idx before DRIVER_OK */\n",
     "    virtio_fence_w_o(); /* staged descriptors and avail->idx before DRIVER_OK */\n"
     "    virtio_fence_o_o();\n"),
    ("mut-stage-descriptor-after-idx", "stage",
     "    q->desc[id].addr = paddr;\n    q->desc[id].len = len;\n"
     "    q->desc[id].flags = flags;\n    q->desc[id].next = 0;\n"
     "    q->avail->ring[q->avail->idx % q->num] = (uint16_t) id;\n"
     "    virtio_mb();\n    q->avail->idx++;",
     "    q->desc[id].flags = flags;\n    q->desc[id].next = 0;\n"
     "    q->avail->ring[q->avail->idx % q->num] = (uint16_t) id;\n"
     "    virtio_mb();\n    q->avail->idx++;\n"
     "    q->desc[id].addr = paddr;\n    q->desc[id].len = len;"),
    ("mut-stage-flags-after-idx", "stage",
     "    q->desc[id].flags = flags;\n    q->desc[id].next = 0;\n"
     "    q->avail->ring[q->avail->idx % q->num] = (uint16_t) id;\n"
     "    virtio_mb();\n    q->avail->idx++;",
     "    q->avail->ring[q->avail->idx % q->num] = (uint16_t) id;\n"
     "    virtio_mb();\n    q->avail->idx++;\n"
     "    q->desc[id].flags = flags;\n    q->desc[id].next = 0;"),
    ("mut-rxstage-wrong-buffer", "rxstage",
     "rtnd->rx_pool.buf[id].paddr", "rtnd->rx_pool.buf[0].paddr"),
    ("mut-rxpost-wrong-flags", "rxpost", "VRING_DESC_F_WRITE", "0"),
    ("mut-stage-no-modulo", "stage", "q->avail->ring[q->avail->idx % q->num]",
     "q->avail->ring[q->avail->idx]"),
    ("mut-stage-kicks", "stage",
     "    q->avail->idx++;\n}", "    q->avail->idx++;\n    virtio_queue_kick(NULL, q);\n}"),
    ("mut-stage-no-barrier", "stage", "    virtio_mb();\n", ""),
    ("mut-stage-idx-before-ring", "stage",
     "    q->avail->ring[q->avail->idx % q->num] = (uint16_t) id;\n    virtio_mb();\n    q->avail->idx++;",
     "    q->avail->idx++;\n    virtio_mb();\n    q->avail->ring[(q->avail->idx - 1) % q->num] = (uint16_t) id;"),
    # The real kick's own ordering (RX and TX both go through it).
    ("mut-kick-no-wo-fence", "kick", "    virtio_fence_w_o();\n", ""),
    ("mut-kick-no-full-barrier", "kick", "    virtio_mb();\n", ""),
    ("mut-kick-wrong-queue", "kick", "q->qid);", "q->qid ^ 1);"),
    ("mut-publish-no-kick", "publish",
     "    virtio_queue_kick(vio, q);\n", "    (void) vio;\n"),
    ("mut-publish-before-check", "init",
     "    int iommu_rc = virtnet_require_iommu_domain(rtnd);",
     "    virtio_queue_publish(&rtnd->vio, rtnd->version, rtnd->rxq);\n"
     "    virtio_queue_publish(&rtnd->vio, rtnd->version, rtnd->txq);\n"
     "    int iommu_rc = virtnet_require_iommu_domain(rtnd);"),
    ("mut-tx-never-published", "init",
     "    virtio_queue_publish(&rtnd->vio, rtnd->version, rtnd->txq);\n", ""),
    ("mut-publish-no-queue-sel", "qpub",
     "    virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_SEL, q->qid);\n", ""),
    ("mut-v2-ready-dropped", "qpub",
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_READY, 1);\n", ""),
    ("mut-alloc-programs-queue", "qalloc",
     "    if (version == 1 && (q->mem_pa >> 12) > UINT32_MAX) {",
     "    virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_NUM, q->num);\n"
     "    if (version == 1 && (q->mem_pa >> 12) > UINT32_MAX) {"),
    ("mut-publish-no-io-fence", "qpub", "    virtio_fence_w_o();\n", ""),
    ("mut-alloc-no-before-sel-fence", "qalloc",
     "    virtio_io_fence_before_sel();\n", ""),
    ("mut-alloc-no-sel-to-numax-fence", "qalloc",
     "    virtio_io_fence_oi();\n", ""),
    ("mut-publish-no-before-sel-fence", "qpub",
     "    virtio_io_fence_before_sel();\n", ""),
    ("mut-publish-no-sel-to-num-fence", "qpub",
     "    virtio_fence_o_o();\n    virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_NUM, q->num);",
     "    virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_NUM, q->num);"),
    ("mut-v1-no-pfn-fence", "qpub",
     "        virtio_fence_o_o();\n        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_PFN,",
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_PFN,"),
    ("mut-v2-no-ready-fence", "qpub",
     "        virtio_fence_o_o();\n        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_READY, 1);",
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_READY, 1);"),
    ("mut-publish-no-trailing-fence", "qpub",
     "    virtio_fence_o_o();\n}", "}"),
    ("mut-publish-fence-too-weak", "qpub",
     "    virtio_io_fence_before_sel();\n", "    virtio_fence_w_o();\n"),
    ("mut-publish-ready-fence-hidden-by-read", "qpub",
     "        virtio_fence_o_o();\n        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_READY, 1);",
     "        (void) virtio_mmio_rd32(vio, VIRTIO_MMIO_STATUS);\n"
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_READY, 1);"),
    ("mut-v1-overflow-off-by-one", "qalloc",
     "(q->mem_pa >> 12) > UINT32_MAX", "(q->mem_pa >> 12) >= UINT32_MAX"),
    ("mut-v1-overflow-unchecked", "qalloc",
     "(q->mem_pa >> 12) > UINT32_MAX", "0"),
    ("mut-v1-pfn-shift", "qpub", "(uint32_t) (q->mem_pa >> 12));", "(uint32_t) (q->mem_pa >> 13));"),
    ("mut-v2-driver-device-swapped", "qpub",
     "VIRTIO_MMIO_QUEUE_DRIVER_LOW,\n                         (uint32_t) q->avail_pa);",
     "VIRTIO_MMIO_QUEUE_DRIVER_LOW,\n                         (uint32_t) q->used_pa);"),
    ("mut-v1-writes-v2-ready", "qpub",
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_ALIGN, 4096);",
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_READY, 1);\n"
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_ALIGN, 4096);"),
    ("mut-v2-writes-v1-pfn", "qpub",
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_READY, 1);\n    }",
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_READY, 1);\n"
     "        virtio_mmio_wr32(vio, VIRTIO_MMIO_QUEUE_PFN, 1);\n    }"),
]


FWO = extract(virtio_c, "virtio_fence_w_o")
FOO = extract(virtio_c, "virtio_fence_o_o")
FSEL = extract(virtio_c, "virtio_io_fence_before_sel")
FOI = extract(virtio_c, "virtio_io_fence_oi")
FENCES = FWO + FOO + FSEL + FOI
WANT = {"virtio_fence_w_o": ["w,o"], "virtio_fence_o_o": ["o,o"],
        "virtio_io_fence_before_sel": ["io,o"], "virtio_io_fence_oi": ["o,i"]}

def riscv_cc():
    """RISCV_CC must be given explicitly: no default, so a missing toolchain fails closed."""
    cc = shlex.split(os.environ.get("RISCV_CC", ""))
    if not cc:
        sys.exit("--riscv-asm requires RISCV_CC, e.g. RISCV_CC='riscv-none-elf-gcc -march=rv64g "
                 "-mabi=lp64d' or 'clang --target=riscv64 -march=rv64g -mabi=lp64d'")
    return cc


def fences(tmp, tag, text):
    """Compile the real fence helpers for RISC-V; return {function: [fence operands]}."""
    c = os.path.join(tmp, tag + ".c")
    with open(c, "w") as f:
        f.write(text)
    cp = subprocess.run(riscv_cc() + ["-ffreestanding", "-O2", "-S", "-o", "-", c],
                        capture_output=True, text=True)
    if cp.returncode:
        sys.exit("%s: RISC-V assembly failed (check RISCV_CC)\n%s" % (tag, cp.stderr))
    out, cur = {}, None
    for line in cp.stdout.splitlines():
        m = re.match(r"^(virtio_(?:io_)?fence_\w+):", line)
        if m:
            cur = out.setdefault(m.group(1), [])
        elif cur is not None and line.strip().startswith("fence"):
            cur.append(re.sub(r"\s+", "", line.strip()[5:]))
    return out


def check_fence_asm(tmp):
    got = fences(tmp, "asm-baseline", FENCES)
    if got != WANT:
        sys.exit("asm-baseline: expected %r, got %r" % (WANT, got))
    print("  %-34s pass" % "asm-baseline")

    def only(name, new):
        parts = {"virtio_fence_w_o": FWO, "virtio_fence_o_o": FOO,
                 "virtio_io_fence_before_sel": FSEL, "virtio_io_fence_oi": FOI}
        parts[name] = new
        return "".join(parts[n] for n in WANT)
    for tag, text in (
            ("asm-mut-wo-full", only("virtio_fence_w_o", mutate(FWO, '"fence w,o"', '"fence rw,rw"'))),
            ("asm-mut-wo-missing", only("virtio_fence_w_o", mutate(
                FWO, '__asm__ volatile("fence w,o" : : : "memory");',
                '__asm__ volatile("" : : : "memory");'))),
            ("asm-mut-wo-fallback", only("virtio_fence_w_o", mutate(
                FWO, "#if defined(__riscv)", "#if 0"))),
            ("asm-mut-oo-ww", only("virtio_fence_o_o", mutate(FOO, '"fence o,o"', '"fence w,w"'))),
            ("asm-mut-oo-swapped", only("virtio_fence_o_o", mutate(FOO, '"fence o,o"', '"fence w,o"'))),
            ("asm-mut-sel-as-oo", only("virtio_io_fence_before_sel", mutate(
                FSEL, '"fence io,o"', '"fence o,o"'))),
            ("asm-mut-oi-as-oo", only("virtio_io_fence_oi", mutate(FOI, '"fence o,i"', '"fence o,o"')))):
        if fences(tmp, tag, text) == WANT:
            sys.exit("%s: mutation not caught by assembly check" % tag)
        print("  %-34s caught" % tag)


def main():
    modes = sys.argv[1:] or ["--host"]
    if len(modes) != 1 or modes[0] not in ("--host", "--riscv-asm"):
        sys.exit("usage: test-lkl-iommu-contract.py [--host | --riscv-asm]")
    if modes[0] == "--riscv-asm":
        with tempfile.TemporaryDirectory() as tmp:
            check_fence_asm(tmp)
        print("lkl-iommu-contract: fence assembly ok")
        return
    with tempfile.TemporaryDirectory() as tmp:
        # IOMMU mutations: the named check must be among the failures.  Q87 ordering
        # mutations: the failing set must equal the listed one exactly, so a mutant
        # cannot be "caught" by an unrelated check.  Names are "function: expr".
        CO = "check_order: "
        SS = "steady_state: "
        expected = {
            "mut-enforcement-unavailable-ok": "main: virtio_mmio_require_iommu_domain(&vio, &one, 1) == ENOTSUP",
            "mut-described-stream-trusted": "main: virtio_mmio_require_iommu_domain(&vio, &one, 1) == ENOTSUP",
            "mut-undescribed-refused": "main: virtio_mmio_require_iommu_domain(&vio, &one, 1) == EOK",
            "mut-init-ignores-refusal": "main: rc == -ENOTSUP",
            "mut-rx-before-check": "main: rx_staged() == 0",
            "mut-driver-ok-before-check": "main: driver_ok == 0",
            "mut-init-kick-per-buffer": {CO + "count_kicks() == 1", CO + "kick > dok",
                                         CO + "dok >= 0 && i > dok",
                                         CO + "kick >= 0 && evs[kick].avail_idx == VIRTNET_Q_SIZE",
                                         "main: bad_order == 0"},   # real notify straight after queue publish
            "mut-init-kick-before-driver-ok": {CO + "mb_before", CO + "count_kicks() == 1",
                                               CO + "kick > dok", CO + "dok >= 0 && i > dok",
                                               "main: bad_order == 0"},
            "mut-init-no-kick": {CO + "count_kicks() == 1", CO + "kick > dok",
                                 CO + "kick >= 0 && evs[kick].avail_idx == VIRTNET_Q_SIZE",
                                 CO + "nev > 0 && evs[nev - 1].t == 'K'"},
            "mut-init-kick-tx": {CO + "evs[i].arg == VIRTNET_RXQ",
                                 CO + "kick >= 0 && evs[kick].avail_idx == VIRTNET_Q_SIZE"},
            "mut-init-no-barrier": {CO + "mb_before"},
            "mut-init-full-barrier-not-wo": {CO + "mb_before"},
            "mut-init-no-oo-fence": {CO + "oo_after"},
            "mut-init-oo-before-status": {CO + "mb_before"},
            "mut-stage-descriptor-after-idx": SS + "evs[0].t != 'M' || evs[0].arg != 5 || (evs[0].desc_addr == 0x5000 && evs[0].desc_len == 64)",
            "mut-stage-flags-after-idx": SS + "evs[0].t != 'M' || evs[0].arg != 5 || (evs[0].desc_flags == VRING_DESC_F_NEXT && evs[0].desc_next == 0)",
            "mut-rxstage-wrong-buffer": CO + "rxq_seen->desc[i].addr == rx_paddr[i]",
            "mut-rxpost-wrong-flags": SS + "rxq_seen->desc[3].flags == VRING_DESC_F_WRITE",
            "mut-stage-no-modulo": SS + "rxq_seen->avail->ring[0] == 3",
            "mut-stage-kicks": {CO + "count_kicks() == 1", CO + "kick > dok",
                                CO + "kick >= 0 && evs[kick].avail_idx == VIRTNET_Q_SIZE",
                                CO + "dok >= 0 && i > dok", "main: bad_order == 0",
                                SS + "count_kicks() == 1"},
            "mut-stage-no-barrier": {SS + "evs[0].t == 'M' && evs[0].arg == 5 && evs[0].avail_idx == 0"},
            "mut-stage-idx-before-ring": {SS + "evs[0].t == 'M' && evs[0].arg == 5 && evs[0].avail_idx == 0"},
            "mut-kick-no-wo-fence": {"check_kicks: i >= 2 && evs[i - 1].t == 'W'"},
            "mut-kick-no-full-barrier": {"check_kicks: evs[i - 2].t == 'M'"},
            "mut-kick-wrong-queue": {
                CO + "evs[i].arg == VIRTNET_RXQ",
                CO + "kick >= 0 && evs[kick].avail_idx == VIRTNET_Q_SIZE",
                SS + "nev > 0 && evs[nev - 1].arg == VIRTNET_TXQ && evs[nev - 1].avail_idx == 1",
                SS + "nev > 0 && evs[nev - 1].avail_idx == VIRTNET_Q_SIZE + 1",
                SS + "nev > 0 && evs[nev - 1].t == 'K' && nev > 0 && evs[nev - 1].arg == VIRTNET_RXQ"},
            "mut-publish-no-kick": {SS + "count_kicks() == 1",
                                 SS + "nev > 0 && evs[nev - 1].arg == VIRTNET_TXQ && evs[nev - 1].avail_idx == 1",
                                 SS + "nev > 0 && evs[nev - 1].avail_idx == VIRTNET_Q_SIZE + 1",
                                 SS + "nev > 0 && evs[nev - 1].t == 'K' && nev > 0 && evs[nev - 1].arg == VIRTNET_RXQ",
                                 SS + "nev >= 3"},
            "mut-publish-before-check": "main: qwrites_at_probe == 0",
            "mut-tx-never-published": "main: dev_version == 2 ? ready_cnt == 2 : pfn_cnt == 2",
            "mut-publish-no-queue-sel": "check_published: reg[i][VIRTIO_MMIO_QUEUE_NUM / 4] == q->num",
            "mut-v2-ready-dropped": "main: dev_version == 2 ? ready_cnt == 2 : pfn_cnt == 2",
            "mut-alloc-programs-queue": "main: qwrites_at_probe == 0",
            "mut-publish-no-io-fence": "main: bad_fence == 0",
            "mut-alloc-no-before-sel-fence": "main: bad_order == 0",
            "mut-alloc-no-sel-to-numax-fence": "main: bad_order == 0",
            "mut-publish-no-before-sel-fence": "main: bad_order == 0",
            "mut-publish-no-sel-to-num-fence": "main: bad_order == 0",
            "mut-v1-no-pfn-fence": "main: bad_order == 0",
            "mut-v2-no-ready-fence": "main: bad_order == 0",
            "mut-publish-no-trailing-fence": "main: bad_order == 0",
            "mut-publish-fence-too-weak": "main: bad_order == 0",
            "mut-publish-ready-fence-hidden-by-read": "main: bad_order == 0",
            "mut-v1-overflow-off-by-one": "main: q != NULL",
            "mut-v1-overflow-unchecked": "main: q == NULL",
            "mut-v1-pfn-shift": "check_published: ((uint64_t) reg[i][VIRTIO_MMIO_QUEUE_PFN / 4] << 12) == q->mem_pa",
            "mut-v2-driver-device-swapped": "check_published: reg64(i, VIRTIO_MMIO_QUEUE_DRIVER_LOW) == q->avail_pa",
            "mut-v1-writes-v2-ready": "check_published: reg[i][VIRTIO_MMIO_QUEUE_READY / 4] == 0",
            "mut-v2-writes-v1-pfn": "check_published: reg[i][VIRTIO_MMIO_QUEUE_PFN / 4] == 0",
        }
        sources = dict(REQ=REQ, INIT=INIT, STAGE=STAGE, PUBLISH=PUBLISH,
                       RXSTAGE=RXSTAGE, RXPOST=RXPOST, QALLOC=QALLOC, QPUB=QPUB, KICK=KICK)
        build_and_run(tmp, "baseline", sources)
        for tag, which, old, new in MUTATIONS:
            mutated = sources.copy()
            key = which.upper()
            mutated[key] = mutate(mutated[key], old, new)
            build_and_run(tmp, tag, mutated, expected[tag])
    print("lkl-iommu-contract: all mutations caught")


main()
