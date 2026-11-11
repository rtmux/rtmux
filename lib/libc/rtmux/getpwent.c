/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * /etc/passwd parser.
 *
 * Implements: setpwent, getpwent, endpwent, getpwnam, getpwuid
 *
 * Format (POSIX, one record per line): name:passwd:uid:gid:gecos:home:shell
 *
 * The returned struct passwd's char* fields point into a static line buffer;
 * subsequent calls overwrite it. Not thread-safe (matches POSIX getpwent).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define PASSWD_PATH "/etc/passwd"
#define PASSWD_LINEMAX 512

static FILE *pw_fp;
static struct passwd pw_entry;
static char pw_line[PASSWD_LINEMAX];

/* Split "s" at the next ':' (or NUL).
 *
 * Returns the start of the field; sets *s to point past the separator (or to
 * the trailing NUL). Replaces ':' with '\0' in place.
 */
static char *next_field(char **sp)
{
    char *start = *sp;
    char *p = start;

    while (*p && *p != ':')
        p++;
    if (*p == ':') {
        *p++ = '\0';
    }
    *sp = p;
    return start;
}

void setpwent(void)
{
    if (pw_fp) {
        fseek(pw_fp, 0L, SEEK_SET);
        clearerr(pw_fp);
        return;
    }
    pw_fp = fopen(PASSWD_PATH, "r");
}

void endpwent(void)
{
    if (pw_fp) {
        fclose(pw_fp);
        pw_fp = NULL;
    }
}

struct passwd *getpwent(void)
{
    char *p, *line, *eol;

    if (!pw_fp) {
        setpwent();
        if (!pw_fp)
            return NULL;
    }

    for (;;) {
        if (!fgets(pw_line, sizeof(pw_line), pw_fp))
            return NULL;

        /* Strip trailing newline (and any \r before it) */
        eol = pw_line + strlen(pw_line);
        while (eol > pw_line && (eol[-1] == '\n' || eol[-1] == '\r'))
            *--eol = '\0';

        /* Skip blank lines and comments */
        if (pw_line[0] == '\0' || pw_line[0] == '#')
            continue;

        /* If the line didn't fit in our buffer (no terminator before EOF and no
         * \n stripped), refuse — partial fields would be worse than a missing
         * entry.
         */
        if (eol == pw_line + sizeof(pw_line) - 1) {
            errno = ERANGE;
            return NULL;
        }

        line = pw_line;
        pw_entry.pw_name = next_field(&line);
        pw_entry.pw_passwd = next_field(&line);

        p = next_field(&line);
        pw_entry.pw_uid = (uid_t) strtoul(p, NULL, 10);

        p = next_field(&line);
        pw_entry.pw_gid = (gid_t) strtoul(p, NULL, 10);

        pw_entry.pw_gecos = next_field(&line);
        pw_entry.pw_dir = next_field(&line);
        pw_entry.pw_shell = next_field(&line);

        /* Fields not populated from /etc/passwd. */
        pw_entry.pw_age = NULL;
        pw_entry.pw_comment = NULL;

        /* A name is the only required field — empty -> skip */
        if (pw_entry.pw_name[0] == '\0')
            continue;

        return &pw_entry;
    }
}

struct passwd *getpwnam(const char *name)
{
    struct passwd *pw;

    if (!name)
        return NULL;
    setpwent();
    while ((pw = getpwent())) {
        if (!strcmp(pw->pw_name, name)) {
            endpwent();
            return pw;
        }
    }
    endpwent();
    return NULL;
}

struct passwd *getpwuid(uid_t uid)
{
    struct passwd *pw;

    setpwent();
    while ((pw = getpwent())) {
        if (pw->pw_uid == uid) {
            endpwent();
            return pw;
        }
    }
    endpwent();
    return NULL;
}

/* Copy the parsed record's strings into the caller's buffer and repoint the
 * caller's struct at them. ERANGE if buf is too small.
 */
static int pw_pack(const struct passwd *src,
                   struct passwd *pwd,
                   char *buf,
                   size_t buflen)
{
    size_t nlen = strlen(src->pw_name) + 1;
    size_t plen = strlen(src->pw_passwd) + 1;
    size_t glen = strlen(src->pw_gecos) + 1;
    size_t dlen = strlen(src->pw_dir) + 1;
    size_t slen = strlen(src->pw_shell) + 1;
    char *b = buf;

    if (nlen + plen + glen + dlen + slen > buflen)
        return ERANGE;

    pwd->pw_name = memcpy(b, src->pw_name, nlen);
    b += nlen;
    pwd->pw_passwd = memcpy(b, src->pw_passwd, plen);
    b += plen;
    pwd->pw_gecos = memcpy(b, src->pw_gecos, glen);
    b += glen;
    pwd->pw_dir = memcpy(b, src->pw_dir, dlen);
    b += dlen;
    pwd->pw_shell = memcpy(b, src->pw_shell, slen);
    pwd->pw_uid = src->pw_uid;
    pwd->pw_gid = src->pw_gid;
    pwd->pw_age = NULL;
    pwd->pw_comment = NULL;
    return 0;
}

/* Reentrant lookup: a private FILE and line buffer, no shared static state. A
 * missing database reads as "not found" (0, *result = NULL), matching a lookup
 * that simply has no match.
 */
static int pw_lookup_r(const char *name,
                       uid_t uid,
                       int by_name,
                       struct passwd *pwd,
                       char *buf,
                       size_t buflen,
                       struct passwd **result)
{
    FILE *fp;
    char line[PASSWD_LINEMAX];

    *result = NULL;
    fp = fopen(PASSWD_PATH, "r");
    if (!fp)
        return (errno == ENOENT) ? 0 : errno;

    while (fgets(line, sizeof line, fp)) {
        struct passwd tmp;
        char *cur = line, *p, *eol;

        eol = line + strlen(line);
        while (eol > line && (eol[-1] == '\n' || eol[-1] == '\r'))
            *--eol = '\0';
        if (eol == line + sizeof line - 1) {
            /* Record longer than the scan buffer: drain the rest of the
             * physical line so its tail is not parsed as a second record.
             */
            int c;
            while ((c = fgetc(fp)) != '\n' && c != EOF)
                ;
            continue;
        }
        if (line[0] == '\0' || line[0] == '#')
            continue;

        tmp.pw_name = next_field(&cur);
        tmp.pw_passwd = next_field(&cur);
        p = next_field(&cur);
        tmp.pw_uid = (uid_t) strtoul(p, NULL, 10);
        p = next_field(&cur);
        tmp.pw_gid = (gid_t) strtoul(p, NULL, 10);
        tmp.pw_gecos = next_field(&cur);
        tmp.pw_dir = next_field(&cur);
        tmp.pw_shell = next_field(&cur);

        if (tmp.pw_name[0] == '\0')
            continue;
        if (by_name ? !strcmp(tmp.pw_name, name) : tmp.pw_uid == uid) {
            int rc = pw_pack(&tmp, pwd, buf, buflen);
            fclose(fp);
            if (rc == 0)
                *result = pwd;
            return rc;
        }
    }
    if (ferror(fp)) { /* a read error is not the same as "not found" */
        int err = errno ? errno : EIO;
        fclose(fp);
        return err;
    }
    fclose(fp);
    return 0;
}

int getpwnam_r(const char *name,
               struct passwd *pwd,
               char *buf,
               size_t buflen,
               struct passwd **result)
{
    if (!result)
        return EINVAL;
    *result = NULL;
    if (!name || !pwd || !buf)
        return EINVAL;
    return pw_lookup_r(name, 0, 1, pwd, buf, buflen, result);
}

int getpwuid_r(uid_t uid,
               struct passwd *pwd,
               char *buf,
               size_t buflen,
               struct passwd **result)
{
    if (!result)
        return EINVAL;
    *result = NULL;
    if (!pwd || !buf)
        return EINVAL;
    return pw_lookup_r(NULL, uid, 0, pwd, buf, buflen, result);
}
