/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_AARCH64_EXCEPTION_H
#define RTMUX_AARCH64_EXCEPTION_H

/* Shared by vectors.S and C. FP/SIMD is disabled in this bootstrap. */
#define A64_FRAME_SP 248
#define A64_FRAME_PC 256
#define A64_FRAME_PSTATE 264
#define A64_FRAME_ESR 272
#define A64_FRAME_FAR 280
#define A64_FRAME_SIZE 288

#ifndef __ASSEMBLY__
#include <stddef.h>
#include <stdint.h>

struct a64_exception_frame {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t esr;
    uint64_t far;
};

_Static_assert(offsetof(struct a64_exception_frame, sp) == A64_FRAME_SP,
               "exception SP offset");
_Static_assert(offsetof(struct a64_exception_frame, pc) == A64_FRAME_PC,
               "exception PC offset");
_Static_assert(offsetof(struct a64_exception_frame, pstate) == A64_FRAME_PSTATE,
               "exception PSTATE offset");
_Static_assert(offsetof(struct a64_exception_frame, esr) == A64_FRAME_ESR,
               "exception ESR offset");
_Static_assert(offsetof(struct a64_exception_frame, far) == A64_FRAME_FAR,
               "exception FAR offset");
_Static_assert(sizeof(struct a64_exception_frame) == A64_FRAME_SIZE,
               "exception frame size");
#endif
#endif
