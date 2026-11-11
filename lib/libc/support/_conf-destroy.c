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

void _conf_destroy(long *list)
{
    long *head;
    long item;

    if ((head = list)) {
        while ((item = *list++) != _CONF_END) {
            switch (item & _CONF_CMD_MASK) {
            case _CONF_VALUE:
                if ((item & _CONF_NAME_MASK) == _CONF_NAME_LONG) {
                    list++;
                }
                if (item & _CONF_STR) {
                    free((void *) *list);
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
    }
    free(head);
}
