/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdlib.h>
#include <string.h>

int getsubopt(char **optionp, char *const *tokens, char **valuep)
{
    int cnt;
    char *p;
    char *subopt;

    *valuep = NULL;

    if (!optionp || !*optionp)
        return (-1);

    /* skip leading white-space, commas */
    for (p = *optionp; *p && (*p == ',' || *p == ' ' || *p == '\t'); ++p)
        ;

    if (!*p) {
        *optionp = p;
        return (-1);
    }

    /* save the start of the token, and skip the rest of the token. */
    for (subopt = p; *++p && *p != ',' && *p != '=' && *p != ' ' && *p != '\t';)
        ;

    if (*p) {
        /* If there's an equals sign, set the value pointer, and skip over the
         * value part of the token. Terminate the token.
         */
        if (*p == '=') {
            *p = '\0';
            for (*valuep = ++p; *p && *p != ',' && *p != ' ' && *p != '\t'; ++p)
                ;
            if (*p)
                *p++ = '\0';
        } else
            *p++ = '\0';
        /* Skip any whitespace or commas after this token. */
        for (; *p && (*p == ',' || *p == ' ' || *p == '\t'); ++p)
            ;
    }

    /* set optionp for next round. */
    *optionp = p;

    for (cnt = 0; *tokens; ++tokens, ++cnt)
        if (!strcmp(subopt, *tokens))
            return (cnt);
    return (-1);
}
