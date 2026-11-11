/*
 * crypttest - SHA-512 crypt(3) known-answer tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <crypt.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    static const struct {
        const char *plain;
        const char *salt;
        const char *expected;
    } vectors[] = {
        {
            "RTMux",
            "$6$rtmuxtest1",
            "$6$rtmuxtest1$Y9rQ.8S6PpteL5Hq1H2pCsyW5HcfMZB7bhQUG4wBa7rikBXO"
            "vQhY5gAUOBgC4Qs0AzS9nGxW2B8Mk0wxjQs7E.",
        },
        {
            "rtmux",
            "$6$rtmuxtest2",
            "$6$rtmuxtest2$v0STB0rbO2isO/Se.Beh5t2V7vxpLykb6moB5vN8hwqYUWVe"
            "vhXsaAGIVuf4agv69sbgVHi1XnppWNUlnrlff.",
        },
    };
    int failures = 0;

    (void) argv;
    if (argc != 1) {
        fprintf(stderr, "usage: crypttest\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        char *got = crypt(vectors[i].plain, vectors[i].salt);

        if (!got || strcmp(got, vectors[i].expected) != 0) {
            printf("crypttest: vector %zu: FAIL\n", i);
            failures++;
        } else {
            printf("crypttest: vector %zu: OK\n", i);
        }
    }
    if (failures == 0) {
        printf("crypttest: all %zu vectors pass.\n",
               sizeof(vectors) / sizeof(vectors[0]));
        return 0;
    }
    printf("crypttest: %d failure(s).\n", failures);
    return 1;
}
