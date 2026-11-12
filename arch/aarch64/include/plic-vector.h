/* SPDX-License-Identifier: Apache-2.0 */
/* Historical name in the public resource ABI: device vectors start at 32.
 * AArch64 Pi3 routes these through BCM2835/2836, not a PLIC. */
#define PLIC_VECTOR_BASE 32
