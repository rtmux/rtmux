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
#include <minmax.h>
#include <sys/conf.h>

int _conf_get(const long *list, int name, long *value, char *str)
{
    long item;

    while ((item = *list++) != _CONF_END) {
        int match;
        int cmd;
        long num;

        switch (cmd = (item & _CONF_CMD_MASK)) {
        case _CONF_NOP:
            break;

        case _CONF_VALUE:
            match = item & _CONF_NAME_MASK;
            if (match == _CONF_NAME_LONG) {
                match = (int) *list++;
            }
            if ((item & (_CONF_NUM | _CONF_STR)) == 0) {
                break;
            }
            if (name != match) {
                list++;
                break;
            }
            /* Fall through */
        case _CONF_CALL:
        case _CONF_LINK:
            num = *list++;
            if (((item & _CONF_STR) && !str) || ((item & _CONF_NUM) && str)) {
                break;
            }
            if (item & _CONF_INDIRECT) {
                num = *(long *) num;
            }
            if (cmd == _CONF_VALUE) {
                if (item & _CONF_NUM) {
                    long i;

                    if (item & _CONF_FCN) {
                        long (*func)(int name);

                        if (!(func = (void *) num)) {
                            break;
                        }
                        num = func(name);
                    }

                    i = *value;
                    switch (item & _CONF_MOD_MASK) {
                    case _CONF_NUM_MIN:
                        *value = (i == -1) ? num : min(i, num);
                        break;
                    case _CONF_NUM_MAX:
                        *value = (i == -1) ? num : max(i, num);
                        break;
                    case _CONF_NUM_ADD:
                        *value = (i == -1) ? num : i + num;
                        break;
                    default:
                        *value = num;
                        return 0;
                    }
                } else {
                    if (item & _CONF_FCN) {
                        size_t (*func)(int name, char *buff, size_t len);

                        if (!(func = (void *) num)) {
                            break;
                        }
                        *value = (long) func(name, str, (size_t) *value);
                    } else {
                        char *ptr = (char *) num;
                        size_t len = strlen(ptr) + 1;
                        size_t buflen;

                        /* PR 12898
                         *
                         * Sanity Check the value, since we're about to pass it
                         * unchecked from min() to strncpy(). Proc calls this
                         * function from it's static lib, and a super large
                         * unsigned number, will become negative for a signed
                         * long. Thus min will return large negative number.
                         */
                        buflen = (*value > 0) ? (size_t) *value : 0;
                        strncpy(str, ptr, min_size(buflen, len));
                        *value = (long) len;
                    }
                    return 0;
                }
                break;
            } else {
                if (item & _CONF_FCN) {
                    int (*func)(int name, long *value, char *str);

                    if ((func = (void *) num)) {
                        int status;

                        if ((status = func(name, value, str)) != -1 ||
                            cmd == _CONF_LINK) {
                            return status;
                        }
                    }
                } else if (cmd == _CONF_LINK) {
                    list = (long *) num;
                } else if (num) {
                    int status;

                    if ((status = _conf_get((long *) num, name, value, str)) !=
                        -1) {
                        return status;
                    }
                }
            }
            break;

        case _CONF_END:
        default:
            return -1;
        }
    }
    return -1;
}
