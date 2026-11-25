/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the production allocator with a VM callback that consumes the
 * predecessor free-list entry while the outer realloc has dropped its lock. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KERNEL_ALLOC_HOST_TEST
#define _MALLOC_ALIGN 16
#define crash() assert(0)
#define min(a, b) ((a) < (b) ? (a) : (b))
enum heap_choice { HEAP_KER, HEAP_KER_CRIT };
static int alloc_slock, alloc_critical;

static void kspinlock_lock(int *held)
{
    assert(!*held);
    *held = 1;
}

static void kspinlock_unlock(int *held)
{
    assert(*held);
    *held = 0;
}

#include "../../kernel/object/alloc.c"

static _Alignas(16) unsigned char arena[512];
static _Alignas(16) unsigned char critical[64];

static size_t grow(size_t n, void **addr)
{
    assert(!alloc_slock && n == 128 + sizeof(struct free_entry));
    void *block = _sreallocfunc(NULL, 0, 32, NULL, HEAP_KER);
    assert(block == arena);
    memset(block, 0xaa, 32);
    *addr = arena + 256;
    return 128;
}

static void check_bytes(const unsigned char *block, int byte)
{
    for (int i = 0; i < 32; i++)
        assert(block[i] == byte);
}

int main(void)
{
    _sreallocfunc(arena, 32, 0, NULL, HEAP_KER);
    memset(arena + 32, 0x55, 32);
    void *new = _sreallocfunc(arena + 32, 32, 128, grow, HEAP_KER);
    assert(new == arena + 256);
    check_bytes(new, 0x55);
    check_bytes(arena, 0xaa);
    assert(_sreallocfunc(NULL, 0, 32, NULL, HEAP_KER) == arena + 32);

    /* A failed grow of a critical block cannot use VM or free its input. */
    memset(critical, 0x55, 32);
    crit_start = (uintptr_t) critical;
    crit_end = crit_start + sizeof critical - 1;
    assert(!_sreallocfunc(critical, 32, 128, grow, HEAP_KER));
    check_bytes(critical, 0x55);
    assert(!alloc_slock);
    puts("kernel-alloc: PASS");
    return 0;
}
