/*
 * Host-side tests for ldso page arithmetic.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <page.h>

size_t page_size;

static void test_4k_pages(void)
{
    page_size = 4096;

    assert(ldso_trunc_page(0) == 0);
    assert(ldso_trunc_page(1) == 0);
    assert(ldso_trunc_page(4095) == 0);
    assert(ldso_trunc_page(4096) == 4096);
    assert(ldso_trunc_page(8191) == 4096);

    assert(ldso_round_page(0) == 0);
    assert(ldso_round_page(1) == 4096);
    assert(ldso_round_page(4095) == 4096);
    assert(ldso_round_page(4096) == 4096);
    assert(ldso_round_page(4097) == 8192);
}

static void test_large_pages(void)
{
    page_size = 2 * 1024 * 1024;

    assert(ldso_trunc_page(0x201000) == 0x200000);
    assert(ldso_round_page(0x201000) == 0x400000);
    assert(ldso_round_page(0x400000) == 0x400000);
}

int main(void)
{
    test_4k_pages();
    test_large_pages();
    return 0;
}
