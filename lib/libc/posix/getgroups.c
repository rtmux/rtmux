/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/rtmux-core.h>

int getgroups(int gidsetsize, gid_t grouplist[])
{
    struct _client_info info;

    connect_client_info(-1, &info, NGROUPS_MAX);
    if (gidsetsize) {
        if (gidsetsize < info.cred.ngroups) {
            errno = EINVAL;
            return -1;
        }
        memcpy(grouplist, info.cred.grouplist,
               info.cred.ngroups * sizeof info.cred.grouplist[0]);
    }
    return (int) info.cred.ngroups;
}
