/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Include the minimum needed so this TU is also compilable from roottask
 * (servers/roottask/Makefile builds sys/walk_asinfo.o from this same source).
 * Pulling kernel/core/globals.h would drag in kopt / kprintf machinery that
 * roottask re-routes through root_log.
 */
#include <string.h>
#include <sys/syspage.h>
#include <log.h> /* LOGF_D — overridden by root-log.h
                                 * when included from roottask */
extern int walk_asinfo(const char *name,
                       int (*func)(struct asinfo_entry *, char *, void *),
                       void *data);

/**
 * \brief Walk asinfo entries in the syspage applying a given function.
 */
int walk_asinfo(const char *name,
                int (*func)(struct asinfo_entry *, char *, void *),
                void *data)
{
    char *str = SYSPAGE_ENTRY(strings)->data;
    struct asinfo_entry *as = SYSPAGE_ENTRY(asinfo);
    char *curr;
    unsigned num;

    num = _syspage_ptr->asinfo.entry_size / sizeof(*as);
    LOGF_D("walk_asinfo: name=%s num=%u str=%p as=%p\n", name ? name : "(null)",
           num, str, as);
    for (;;) {
        if (num == 0)
            return 1;
        curr = &str[as->name];
        LOGF_D("  as[%u]: start=%lx end=%lx name=%s\n",
               _syspage_ptr->asinfo.entry_size / sizeof(*as) - num,
               (unsigned long) as->start, (unsigned long) as->end, curr);
        if (!name || !strcmp(curr, name)) {
            LOGF_D("  -> calling func %lx\n", (unsigned long) func);
            if (!func(as, curr, data))
                return 0;
        }
        ++as;
        --num;
    }
}
