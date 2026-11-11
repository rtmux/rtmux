/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <sys/conf.h>

int _conf_set(long **plist, int cmd, int name, long value, const char *str)
{
    long *list = *plist;
    int pos = 0;

    if ((list = *plist)) {
        long item;

        while ((item = *list++) != _CONF_END) {
            int match;

            switch (item & _CONF_CMD_MASK) {
            case _CONF_VALUE:

                match = item & _CONF_NAME_MASK;
                if (match == _CONF_NAME_LONG) {
                    match = (int) *list++;
                }
                if (match == name) {
                    if (str && ((unsigned) item & _CONF_STR)) {
                        char *p;

                        if (!(p = realloc((char **) *list,
                                          (size_t) value + 1))) {
                            return ENOMEM;
                        }
                        strncpy(p, str, (size_t) value);
                        p[value] = '\0';
                        *(char **) list = p;
                        return EOK;
                    }
                    if (!str && ((unsigned) item & _CONF_NUM)) {
                        *list = value;
                        return EOK;
                    }
                }
                /* Fall through */
            case _CONF_CALL:
            case _CONF_LINK:
                list++;
                break;
            default:
                break;
            }
        }
        pos = (int) ((list - *plist) - 1);
    }

    if (str) {
        char *s;

        if (!((unsigned) cmd & _CONF_STR)) {
            return EINVAL;
        }
        if (!(s = malloc((size_t) value + 1))) {
            return ENOMEM;
        }
        strncpy(s, str, (size_t) value);
        s[value] = '\0';
        value = (intptr_t) s;
    } else if (!((unsigned) cmd & _CONF_NUM)) {
        return EINVAL;
    }

    if (!(list = realloc(*plist,
                         (size_t) (pos + (name >= _CONF_NAME_LONG ? 4 : 3)) *
                             sizeof *list))) {
        if (str) {
            free((void *) value);
        }
        return ENOMEM;
    }
    *plist = list;
    list += pos;

    if (name >= _CONF_NAME_LONG) {
        list[1] = name;
        list[2] = value;
        list[3] = _CONF_END;
        name = _CONF_NAME_LONG;
    } else {
        list[1] = value;
        list[2] = _CONF_END;
    }
    list[0] = (long) ((unsigned) name | ((unsigned) cmd & ~_CONF_NAME_LONG) |
                      _CONF_VALUE);
    return EOK;
}
