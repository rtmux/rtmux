/*
 * Standalone driver for the fuzz harnesses.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* libFuzzer supplies its own main and drives LLVMFuzzerTestOneInput. Where only
 * the sanitizers exist (a clang without the libFuzzer runtime), this main reads
 * each path argument once and feeds it to the harness, so the same target does
 * ASan and UBSan corpus replay. A harness links either this main or libFuzzer,
 * never both.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        long n;
        uint8_t *buf;
        size_t got;

        if (!f) {
            perror(argv[i]);
            return 1;
        }
        if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
            fclose(f);
            return 1;
        }
        rewind(f);
        buf = malloc((size_t) n + 1);
        if (!buf) {
            fclose(f);
            return 1;
        }
        got = fread(buf, 1, (size_t) n, f);
        if (ferror(f) || got != (size_t) n) {
            fprintf(stderr, "%s: short read\n", argv[i]);
            fclose(f);
            free(buf);
            return 1;
        }
        fclose(f);
        LLVMFuzzerTestOneInput(buf, got);
        free(buf);
    }
    return 0;
}
