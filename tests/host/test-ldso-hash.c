/*
 * Host-side tests for ldso ELF hash helpers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdint.h>

#include <hash.h>

int main(void)
{
    assert(elf_hash("") == 0x00000000);
    assert(elf_hash("printf") == 0x077905a6);
    assert(elf_hash("dlopen") == 0x06b366be);

    assert(ldso_gnu_hash("") == 0x00001505);
    assert(ldso_gnu_hash("printf") == UINT32_C(0x156b2bb8));
    assert(ldso_gnu_hash("dlopen") == UINT32_C(0xf9040207));

    return 0;
}
