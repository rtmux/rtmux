/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Queue submission and completion polling live in nvme_exec_cmd()
 * (nvme-ctrl.c). This file just builds the Identify SQEs and unpacks the
 * results.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>

#include "nvme.h"

/* Single persistent Identify bounce buffer. Allocating a fresh page for every
 * Identify and munmap'ing it turned up a flaky-at-rates failure mode where the
 * next call would DMA into what looked like its own page but the CPU-side reads
 * still saw the previous call's bytes — most likely physical-page reuse with
 * stale cache state. One page, allocated once, zeroed on every entry, sidesteps
 * the whole class of problems and matches the NVMe driver idiom used by Linux
 * (cache of DMA-able scratch buffers, not alloc-per-command).
 */
static uint8_t *ident_buf;
static uint64_t ident_buf_pa;

static int ident_buf_init(void)
{
    off_t pa;
    if (ident_buf)
        return 0;
    ident_buf =
        mmap(NULL, NVME_IDENT_BUF_SIZE, PROT_READ | PROT_WRITE | PROT_NOCACHE,
             MAP_ANON | MAP_SHARED, NOFD, 0);
    if (ident_buf == MAP_FAILED) {
        fprintf(stderr, "nvme: mmap identify buf: %s\n", strerror(errno));
        ident_buf = NULL;
        return -1;
    }
    if (mem_offset(ident_buf, NOFD, NVME_IDENT_BUF_SIZE, &pa, NULL) == -1) {
        fprintf(stderr, "nvme: mem_offset identify buf: %s\n", strerror(errno));
        munmap(ident_buf, NVME_IDENT_BUF_SIZE);
        ident_buf = NULL;
        return -1;
    }
    ident_buf_pa = (uint64_t) pa;
    return 0;
}

/* Build and execute an Identify command. The DMA buffer is the module-static
 * ident_buf; on success the caller-supplied <out> receives a copy.
 */
static int nvme_identify(nvme_ctrl_t *c,
                         uint32_t nsid,
                         uint32_t cns,
                         void *out,
                         size_t out_len)
{
    if (ident_buf_init() != 0)
        return -1;
    memset(ident_buf, 0, NVME_IDENT_BUF_SIZE);

    nvme_sqe_t cmd = {
        .opc = NVME_ADMIN_OP_IDENTIFY,
        .nsid = nsid,
        .prp1 = ident_buf_pa,
        .cdw10 = cns & 0xffu,
    };

    int sf = nvme_exec_cmd(c, 0, &cmd);
    if (sf < 0)
        return -1;
    if (sf != 0) {
        fprintf(stderr,
                "nvme: Identify CNS=0x%x NSID=%u returned status 0x%x\n", cns,
                nsid, (unsigned) sf);
        return -1;
    }
    memcpy(out, ident_buf, out_len);
    return 0;
}

int nvme_identify_controller(nvme_ctrl_t *c, nvme_id_ctrl_t *out)
{
    return nvme_identify(c, 0, NVME_IDENT_CNS_CONTROLLER, out, sizeof(*out));
}

int nvme_identify_active_ns_list(nvme_ctrl_t *c, uint32_t *nsids)
{
    return nvme_identify(c, 0, NVME_IDENT_CNS_ACTIVE_NS_LIST, nsids,
                         1024u * sizeof(uint32_t));
}

int nvme_identify_namespace(nvme_ctrl_t *c, uint32_t nsid, nvme_id_ns_t *out)
{
    return nvme_identify(c, nsid, NVME_IDENT_CNS_NAMESPACE, out, sizeof(*out));
}
