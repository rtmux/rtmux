/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * /etc/group parser.
 *
 * Implements: setgrent, getgrent, endgrent, getgrnam, getgrgid
 *
 * Format (POSIX, one record per line): name:passwd:gid:user1,user2,...
 *
 * The members list is split in place; gr_mem points into a static pointer array
 * that gets rebuilt on every call. Same trade-offs as getpwent: not
 * thread-safe; reentrant variants TBD.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#define GROUP_PATH "/etc/group"
#define GROUP_LINEMAX 512
#define GROUP_MAXMEMBERS 64 /* members per group */

static FILE *gr_fp;
static struct group gr_entry;
static char gr_line[GROUP_LINEMAX];
static char *gr_members[GROUP_MAXMEMBERS + 1]; /* +1 for NULL terminator */

static char *next_field(char **sp)
{
    char *start = *sp;
    char *p = start;

    while (*p && *p != ':')
        p++;
    if (*p == ':')
        *p++ = '\0';
    *sp = p;
    return start;
}

void setgrent(void)
{
    if (gr_fp) {
        fseek(gr_fp, 0L, SEEK_SET);
        clearerr(gr_fp);
        return;
    }
    gr_fp = fopen(GROUP_PATH, "r");
}

void endgrent(void)
{
    if (gr_fp) {
        fclose(gr_fp);
        gr_fp = NULL;
    }
}

struct group *getgrent(void)
{
    char *p, *line, *eol, *members;
    size_t nmem;

    if (!gr_fp) {
        setgrent();
        if (!gr_fp)
            return NULL;
    }

    for (;;) {
        if (!fgets(gr_line, sizeof(gr_line), gr_fp))
            return NULL;

        eol = gr_line + strlen(gr_line);
        while (eol > gr_line && (eol[-1] == '\n' || eol[-1] == '\r'))
            *--eol = '\0';

        if (gr_line[0] == '\0' || gr_line[0] == '#')
            continue;

        if (eol == gr_line + sizeof(gr_line) - 1) {
            errno = ERANGE;
            return NULL;
        }

        line = gr_line;
        gr_entry.gr_name = next_field(&line);
        gr_entry.gr_passwd = next_field(&line);

        p = next_field(&line);
        gr_entry.gr_gid = (gid_t) strtoul(p, NULL, 10);

        /* Members: comma-separated.  Split in place. */
        members = next_field(&line);
        nmem = 0;
        if (*members) {
            char *m = members;
            char *q;

            gr_members[nmem++] = m;
            for (q = m; *q; q++) {
                if (*q == ',') {
                    *q = '\0';
                    if (nmem >= GROUP_MAXMEMBERS) {
                        errno = ERANGE;
                        return NULL;
                    }
                    gr_members[nmem++] = q + 1;
                }
            }
        }
        gr_members[nmem] = NULL;
        gr_entry.gr_mem = gr_members;

        if (gr_entry.gr_name[0] == '\0')
            continue;

        return &gr_entry;
    }
}

struct group *getgrnam(const char *name)
{
    struct group *gr;

    if (!name)
        return NULL;
    setgrent();
    while ((gr = getgrent())) {
        if (!strcmp(gr->gr_name, name)) {
            endgrent();
            return gr;
        }
    }
    endgrent();
    return NULL;
}

struct group *getgrgid(gid_t gid)
{
    struct group *gr;

    setgrent();
    while ((gr = getgrent())) {
        if (gr->gr_gid == gid) {
            endgrent();
            return gr;
        }
    }
    endgrent();
    return NULL;
}

/* Pack the matched record into the caller's buffer: an aligned char* array for
 * gr_mem (nmem entries plus a NULL terminator), then all the strings. ERANGE if
 * buf cannot hold them.
 */
static int gr_pack(const struct group *src,
                   char *const *mem,
                   size_t nmem,
                   struct group *grp,
                   char *buf,
                   size_t buflen)
{
    size_t align = _Alignof(char *);
    uintptr_t base = (uintptr_t) buf;
    uintptr_t aligned = (base + align - 1) & ~(uintptr_t) (align - 1);
    size_t pad = (size_t) (aligned - base);
    size_t ptrbytes = (nmem + 1) * sizeof(char *);
    size_t nlen = strlen(src->gr_name) + 1;
    size_t plen = strlen(src->gr_passwd) + 1;
    size_t need = pad + ptrbytes + nlen + plen;
    size_t i;
    char **arr;
    char *s;

    for (i = 0; i < nmem; i++)
        need += strlen(mem[i]) + 1;
    if (need > buflen)
        return ERANGE;

    arr = (char **) (buf + pad);
    s = buf + pad + ptrbytes;
    grp->gr_name = memcpy(s, src->gr_name, nlen);
    s += nlen;
    grp->gr_passwd = memcpy(s, src->gr_passwd, plen);
    s += plen;
    for (i = 0; i < nmem; i++) {
        size_t len = strlen(mem[i]) + 1;
        arr[i] = memcpy(s, mem[i], len);
        s += len;
    }
    arr[nmem] = NULL;
    grp->gr_mem = arr;
    grp->gr_gid = src->gr_gid;
    return 0;
}

/* Reentrant lookup: private FILE, line buffer, and member-pointer array; a
 * missing database reads as "not found".
 */
static int gr_lookup_r(const char *name,
                       gid_t gid,
                       int by_name,
                       struct group *grp,
                       char *buf,
                       size_t buflen,
                       struct group **result)
{
    FILE *fp;
    char line[GROUP_LINEMAX];
    char *mem[GROUP_MAXMEMBERS];

    *result = NULL;
    fp = fopen(GROUP_PATH, "r");
    if (!fp)
        return (errno == ENOENT) ? 0 : errno;

    while (fgets(line, sizeof line, fp)) {
        struct group tmp;
        char *cur = line, *p, *eol, *members;
        size_t nmem = 0;

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

        tmp.gr_name = next_field(&cur);
        tmp.gr_passwd = next_field(&cur);
        p = next_field(&cur);
        tmp.gr_gid = (gid_t) strtoul(p, NULL, 10);

        if (tmp.gr_name[0] == '\0')
            continue;
        if (by_name ? strcmp(tmp.gr_name, name) != 0 : tmp.gr_gid != gid)
            continue;

        members = next_field(&cur);
        if (*members) {
            char *m = members, *q;

            mem[nmem++] = m;
            for (q = m; *q; q++) {
                if (*q == ',') {
                    *q = '\0';
                    if (nmem >= GROUP_MAXMEMBERS) {
                        fclose(fp);
                        return ERANGE; /* too many members to represent */
                    }
                    mem[nmem++] = q + 1;
                }
            }
        }

        int rc = gr_pack(&tmp, mem, nmem, grp, buf, buflen);
        fclose(fp);
        if (rc == 0)
            *result = grp;
        return rc;
    }
    if (ferror(fp)) { /* a read error is not the same as "not found" */
        int err = errno ? errno : EIO;
        fclose(fp);
        return err;
    }
    fclose(fp);
    return 0;
}

int getgrnam_r(const char *name,
               struct group *grp,
               char *buf,
               size_t buflen,
               struct group **result)
{
    if (!result)
        return EINVAL;
    *result = NULL;
    if (!name || !grp || !buf)
        return EINVAL;
    return gr_lookup_r(name, 0, 1, grp, buf, buflen, result);
}

int getgrgid_r(gid_t gid,
               struct group *grp,
               char *buf,
               size_t buflen,
               struct group **result)
{
    if (!result)
        return EINVAL;
    *result = NULL;
    if (!grp || !buf)
        return EINVAL;
    return gr_lookup_r(NULL, gid, 0, grp, buf, buflen, result);
}
