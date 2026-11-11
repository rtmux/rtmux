/* SPDX-License-Identifier: BSD-3-Clause */

#include <string.h>
#include <ctype.h>

int strcasecmp(const char *s1, const char *s2)
{
    const unsigned char *us1 = (const unsigned char *) s1,
                        *us2 = (const unsigned char *) s2;

    while (tolower(*us1) == tolower(*us2++))
        if (*us1++ == '\0')
            return 0;
    return tolower(*us1) - tolower(*--us2);
}

int strncasecmp(const char *s1, const char *s2, size_t n)
{
    if (n != 0) {
        const unsigned char *us1 = (const unsigned char *) s1,
                            *us2 = (const unsigned char *) s2;

        do {
            if (tolower(*us1) != tolower(*us2++))
                return tolower(*us1) - tolower(*--us2);
            if (*us1++ == '\0')
                break;
        } while (--n != 0);
    }
    return 0;
}
