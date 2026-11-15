/*
 * getlogin / getlogin_r -- the terminal login name.
 *
 * RTMux keeps no utmp; the login program exports the login name as $LOGNAME
 * (see userland/utils/toolbox.c), which stays the "who logged in" value even
 * after a su changes the effective uid. getlogin_r therefore reads $LOGNAME,
 * falling back to the current uid's passwd name so a process outside a login
 * session still gets a sensible answer. getlogin_r is reentrant (caller buffer,
 * no static state); getlogin() is the non-reentrant wrapper.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "env-lock.h"

/* Copy src into name[namesize], or ERANGE. */
static int copy_out(char *name, size_t namesize, const char *src)
{
    size_t len = strlen(src);

    if (len + 1 > namesize)
        return ERANGE;
    memcpy(name, src, len + 1);
    return 0;
}

int getlogin_r(char *name, size_t namesize)
{
    const char *login;
    struct passwd pw, *result;
    char pwbuf[256];
    int rc = -1;

    /* Copy $LOGNAME under the environment lock: getlogin_r is thread-safe, and
     * a concurrent setenv may rewrite the value in place.
     */
    __env_lock();
    login = getenv("LOGNAME");
    if (login && *login)
        rc = copy_out(name, namesize, login);
    __env_unlock();
    if (rc >= 0)
        return rc;

    rc = getpwuid_r(getuid(), &pw, pwbuf, sizeof pwbuf, &result);
    if (rc != 0)
        return rc;
    if (!result)
        return ENXIO; /* no passwd entry for the current uid */
    return copy_out(name, namesize, result->pw_name);
}

char *getlogin(void)
{
    static char buf[256];
    int rc = getlogin_r(buf, sizeof buf);

    if (rc != 0) {
        errno = rc;
        return NULL;
    }
    return buf;
}
