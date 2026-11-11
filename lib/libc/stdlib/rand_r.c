/*
 * rand_r -- reentrant pseudo-random number generator.
 *
 * The caller owns the seed, so there is no shared state and the function is
 * thread-safe (unlike rand()/srand(), which RTMux does not provide). A simple
 * linear congruential generator returning the top 15 bits, so the result lies
 * in [0, RAND_MAX] with RAND_MAX == 0x7fff, matching <stdlib.h>.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

int rand_r(unsigned int *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (int) ((*seed >> 16) & 0x7fffu);
}
