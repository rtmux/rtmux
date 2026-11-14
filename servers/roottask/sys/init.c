/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <alloca.h>
#include <sys/conf.h>
#include <sys/syspage.h>
#include <rtmux-version.h>

static int roottask_confstr_seed(int cmd, int name, const char *str)
{
    size_t len = strlen(str);

    if (strchr(str, ' ')) {
        char *p1, *s = 0;
        const char *p2;

        p1 = alloca(len + 1);
        if (!p1) {
            return ENOMEM;
        }

        for (s = p1, p2 = str; (*p1 = *p2); p1++, p2++) {
            if (*p1 == ' ') {
                *p1 = '_';
            }
        }
        str = (const char *) s;
    }
    return roottask_sysconf_set(ROOTTASK_PID, _CONF_STR | (unsigned) cmd, name,
                                (long) strlen(str), str);
}

/* This is here to make internal calls to sysconf() not send a message to the
 * proc service.
 */
static const long roottask_internal_table[] = {
    _CONF_VALUE_NUM | _CONF_INDIRECT | _SC_PAGESIZE,
    (intptr_t) &root_vm_ops.pagesize, _CONF_END};


/* The syspage build_info record, or NULL when the section is absent, shorter
 * than this roottask expects, or outside the syspage. The strings are NOT
 * guaranteed to terminate: callers must bound them (%.63s / %.15s, memcpy).
 */
const struct build_info_entry *syspage_build_info(void)
{
    return syspage_build_info_of(_syspage_ptr);
}

/* Kernel and roottask are linked from one tree, so a differing build id means
 * the boot image mixes builds (a kernel from one tree, an initramfs from
 * another). Say so once, naming both ids.
 */
static void roottask_version_check(void)
{
    const struct build_info_entry *bi = syspage_build_info();
    char kid[sizeof(bi->build_id) + 1] = "unknown";

    if (bi) {
        memcpy(kid, bi->build_id, sizeof(bi->build_id));
        kid[sizeof(bi->build_id)] = '\0';
    }
    if (strcmp(kid, RTMUX_BUILD_ID) != 0)
        root_log("version skew: kernel=%s roottask=%s\n", kid, RTMUX_BUILD_ID);
}

void roottask_conf_init(void)
{
    _sysconf_list = (long *) &roottask_internal_table;

    /* "os_version_string" defined in automatically created timestamp.c file,
     * value determined by the RTMUX_VERSION macro in <rtmux-version.h>
     */
    roottask_confstr_seed(0, _CS_RELEASE, os_version_string);

    /* "timestamp" defined in automatically created timestamp.c file, value
     * determined by time of roottask link.
     */
    roottask_confstr_seed(0, _CS_VERSION, timestamp);

    roottask_confstr_seed(0, _CS_ARCHITECTURE, CPU_STRINGNAME);

    roottask_version_check();
}
