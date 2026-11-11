/*
 * toolbox sh - compact command shell applet.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "toolbox.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static char **sh_environ(void)
{
    return environ;
}

#define SH_MAX_TOKENS 128

/* Widest source line load_script keeps intact; a longer line is force-split
 * (with a diagnostic). Sized to fit the longest init/sysinit line with
 * headroom.
 */
#define SH_LINE_MAX 2048
#define SH_HISTORY_MAX 16

enum sh_op {
    SH_WORD,
    SH_SEMI,
    SH_PIPE,
    SH_AND,
    SH_OR,
    SH_BG,
    SH_LT,
    SH_GT,
    SH_GTGT,
    SH_ERRGT,
    SH_ERRGTGT,
};

struct sh_token {
    char *s;
    enum sh_op op;
};

struct sh_words {
    char **v;
    int n;
    int cap;
};

struct sh_script {
    char **line;
    int count;
    int cap;
};

struct sh_func {
    char *name;
    struct sh_script body;
};

struct sh {
    bool interactive;
    int last_status;
    bool exit_requested;
    char *history[SH_HISTORY_MAX];
    int history_count;
    char **params; /* positional parameters $1..$nparams */
    int nparams;
    char *arg0;       /* $0 */
    bool opt_errexit; /* set -e: exit on a failed command (see errexit_suppress)
                       */
    bool opt_nounset; /* set -u: unset-parameter reference is an error */
    bool opt_xtrace;  /* set -x: trace expanded commands to stderr */
    int loop_break;   /* pending `break N` levels */
    int loop_continue; /* pending `continue N` levels */
    int loop_depth; /* enclosing for/while loops (break/continue no-op at 0) */
    struct sh_func *funcs; /* defined shell functions */
    int nfuncs;
    int funcs_cap;
    bool return_requested; /* `return` unwinds the running function */
    int return_status;
    int fn_depth;      /* enclosing function calls (`return` no-op at 0) */
    int heredoc_fd;    /* pending here-document body on stdin, else -1 */
    bool expand_abort; /* a `set -u` failure: skip the command being expanded */
    int errexit_suppress; /* >0 while in a context where `set -e` is ignored */
};

enum sh_key {
    SH_KEY_NULL = 0,
    SH_KEY_ESC = 27,
    SH_KEY_BACKSPACE = 127,
    SH_KEY_ARROW_LEFT = 1000,
    SH_KEY_ARROW_RIGHT,
    SH_KEY_ARROW_UP,
    SH_KEY_ARROW_DOWN,
    SH_KEY_DELETE,
    SH_KEY_HOME,
    SH_KEY_END,
};

static int run_file(struct sh *sh, const char *path);
static int run_line(struct sh *sh, const char *line);
static struct sh_func *sh_find_func(struct sh *sh, const char *name);
static int run_function(struct sh *sh,
                        struct sh_func *fn,
                        char **argv,
                        int argc);

static const char *const sh_builtin_names[] = {
    ":",    ".",      "break",   "cd",     "command", "continue", "echo",
    "eval", "exec",   "exit",    "export", "false",   "help",     "pwd",
    "read", "return", "set",     "shift",  "source",  "test",     "true",
    "type", "unset",  "waitfor", "[",      "[[",      "jobs",     "fg",
    "bg",   NULL,
};

static void sh_free_history(struct sh *sh)
{
    for (int i = 0; i < sh->history_count; i++)
        free(sh->history[i]);
    sh->history_count = 0;
}

static void sh_add_history(struct sh *sh, const char *line)
{
    char *copy;

    if (line[0] == '\0')
        return;
    if (sh->history_count > 0 &&
        !strcmp(sh->history[sh->history_count - 1], line))
        return;
    copy = strdup(line);
    if (!copy)
        return;
    if (sh->history_count == SH_HISTORY_MAX) {
        free(sh->history[0]);
        memmove(&sh->history[0], &sh->history[1],
                (SH_HISTORY_MAX - 1) * sizeof(sh->history[0]));
        sh->history_count--;
    }
    sh->history[sh->history_count++] = copy;
}

static void sh_write_all(const char *s)
{
    size_t len = strlen(s);

    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, s, len);

        if (n <= 0)
            return;
        s += n;
        len -= (size_t) n;
    }
}

static int sh_enable_raw(struct termios *saved)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, saved) != 0)
        return -1;
    raw = *saved;
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t) ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static int sh_read_key(void)
{
    char c;
    char seq[3];

    if (read(STDIN_FILENO, &c, 1) != 1)
        return SH_KEY_NULL;
    if (c != '\033')
        return (unsigned char) c;
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
        return SH_KEY_ESC;
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
        return SH_KEY_ESC;
    if (seq[0] != '[')
        return SH_KEY_ESC;
    if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
            return SH_KEY_ESC;
        if (seq[2] == '~') {
            switch (seq[1]) {
            case '1':
            case '7':
                return SH_KEY_HOME;
            case '3':
                return SH_KEY_DELETE;
            case '4':
            case '8':
                return SH_KEY_END;
            default:
                break;
            }
        }
        return SH_KEY_ESC;
    }
    switch (seq[1]) {
    case 'A':
        return SH_KEY_ARROW_UP;
    case 'B':
        return SH_KEY_ARROW_DOWN;
    case 'C':
        return SH_KEY_ARROW_RIGHT;
    case 'D':
        return SH_KEY_ARROW_LEFT;
    case 'H':
        return SH_KEY_HOME;
    case 'F':
        return SH_KEY_END;
    default:
        return SH_KEY_ESC;
    }
}

static void sh_refresh_line(const char *prompt, const char *buf, size_t pos)
{
    char seq[48];

    sh_write_all("\r");
    sh_write_all(prompt);
    sh_write_all(buf);
    sh_write_all("\x1b[K");
    snprintf(seq, sizeof(seq), "\r\x1b[%uC", (unsigned) (strlen(prompt) + pos));
    sh_write_all(seq);
}

static int sh_starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int sh_match_add(char matches[][PATH_MAX],
                        int count,
                        const char *name,
                        const char *prefix)
{
    for (int i = 0; i < count; i++) {
        if (!strcmp(matches[i], name))
            return count;
    }
    if (count >= 64 || !sh_starts_with(name, prefix))
        return count;
    snprintf(matches[count], PATH_MAX, "%s", name);
    return count + 1;
}

static int sh_complete_word(const char *prefix, char matches[][PATH_MAX])
{
    char *path;
    char *save;
    char *dir;
    int count = 0;

    for (int i = 0; sh_builtin_names[i]; i++)
        count = sh_match_add(matches, count, sh_builtin_names[i], prefix);
    for (const struct toolbox_applet *ap = toolbox_applets(); ap->name; ap++)
        count = sh_match_add(matches, count, ap->name, prefix);

    path = strdup(getenv("PATH") ? getenv("PATH") : "/bin:/sbin");
    if (!path)
        return count;
    for (dir = strtok_r(path, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
        DIR *dp = opendir(*dir != '\0' ? dir : ".");
        struct dirent *de;

        if (!dp)
            continue;
        while ((de = readdir(dp)))
            count = sh_match_add(matches, count, de->d_name, prefix);
        closedir(dp);
    }
    free(path);
    return count;
}

static int sh_complete_path(const char *prefix, char matches[][PATH_MAX])
{
    char dirbuf[PATH_MAX];
    char namebuf[PATH_MAX];
    const char *slash;
    const char *dir;
    const char *name;
    DIR *dp;
    struct dirent *de;
    int count = 0;

    slash = strrchr(prefix, '/');
    if (slash) {
        size_t dirlen = (size_t) (slash - prefix);

        if (dirlen == 0)
            snprintf(dirbuf, sizeof(dirbuf), "/");
        else {
            if (dirlen >= sizeof(dirbuf))
                return 0;
            memcpy(dirbuf, prefix, dirlen);
            dirbuf[dirlen] = '\0';
        }
        dir = dirbuf;
        name = slash + 1;
    } else {
        dir = ".";
        name = prefix;
    }

    /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
    dp = opendir(dir);
    if (!dp)
        return 0;
    while ((de = readdir(dp))) {
        struct stat st;
        char full[PATH_MAX];

        if (!sh_starts_with(de->d_name, name))
            continue;
        if (slash && !strcmp(dir, "/"))
            snprintf(namebuf, sizeof(namebuf), "/%s", de->d_name);
        else if (slash)
            snprintf(namebuf, sizeof(namebuf), "%s/%s", dir, de->d_name);
        else
            snprintf(namebuf, sizeof(namebuf), "%s", de->d_name);
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode) &&
            strlen(namebuf) + 1 < sizeof(namebuf))
            strcat(namebuf, "/");
        count = sh_match_add(matches, count, namebuf, prefix);
    }
    closedir(dp);
    return count;
}

static void sh_apply_completion(const char *prompt,
                                char *buf,
                                size_t *len,
                                size_t *pos)
{
    char matches[64][PATH_MAX];
    size_t start = *pos;
    char prefix[PATH_MAX];
    int command_word = 1;
    int count;

    while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t' &&
           buf[start - 1] != ';' && buf[start - 1] != '|')
        start--;
    for (size_t i = 0; i < start; i++) {
        if (buf[i] != ' ' && buf[i] != '\t') {
            command_word = 0;
            break;
        }
    }
    if (*pos - start >= sizeof(prefix))
        return;
    memcpy(prefix, buf + start, *pos - start);
    prefix[*pos - start] = '\0';
    if (command_word && !strchr(prefix, '/'))
        count = sh_complete_word(prefix, matches);
    else
        count = sh_complete_path(prefix, matches);
    if (count == 1) {
        size_t old = *pos - start;
        size_t add = strlen(matches[0]) - old;

        if (strlen(matches[0]) < old || *len + add + 2 >= SH_LINE_MAX)
            return;
        memmove(buf + *pos + add, buf + *pos, *len - *pos + 1);
        memcpy(buf + start, matches[0], strlen(matches[0]));
        *len += add;
        *pos += add;
        if (*len + 2 < SH_LINE_MAX) {
            memmove(buf + *pos + 1, buf + *pos, *len - *pos + 1);
            buf[*pos] = ' ';
            (*pos)++;
            (*len)++;
        }
        return;
    }
    if (count > 1) {
        sh_write_all("\r\n");
        for (int i = 0; i < count; i++) {
            sh_write_all(matches[i]);
            sh_write_all(i + 1 == count ? "\r\n" : "  ");
        }
        sh_refresh_line(prompt, buf, *pos);
    }
}

static int sh_read_line(struct sh *sh,
                        const char *prompt,
                        char *out,
                        size_t outsz)
{
    struct termios saved;
    char buf[SH_LINE_MAX];
    size_t len = 0;
    size_t pos = 0;
    int hist = sh->history_count;
    int raw;

    if (outsz == 0)
        return -1;
    raw = sh->interactive && sh_enable_raw(&saved) == 0;
    if (!raw) {
        fputs(prompt, stdout);
        fflush(stdout);
        if (!fgets(out, (int) outsz, stdin))
            return -1;
        return 0;
    }
    buf[0] = '\0';
    sh_write_all(prompt);
    for (;;) {
        int c = sh_read_key();

        if (c == SH_KEY_NULL || c == 4) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
            return -1;
        }
        if (c == '\r' || c == '\n') {
            sh_write_all("\r\n");
            break;
        }
        if (c == '\t') {
            sh_apply_completion(prompt, buf, &len, &pos);
        } else if (c == SH_KEY_ARROW_LEFT || c == 2) {
            if (pos > 0)
                pos--;
        } else if (c == SH_KEY_ARROW_RIGHT || c == 6) {
            if (pos < len)
                pos++;
        } else if (c == SH_KEY_HOME || c == 1) {
            pos = 0;
        } else if (c == SH_KEY_END || c == 5) {
            pos = len;
        } else if (c == 21) {
            memmove(buf, buf + pos, len - pos + 1);
            len -= pos;
            pos = 0;
        } else if (c == 11) {
            buf[pos] = '\0';
            len = pos;
        } else if (c == 12) {
            sh_write_all("\x1b[2J\x1b[H");
        } else if (c == SH_KEY_BACKSPACE || c == '\b') {
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos + 1);
                pos--;
                len--;
            }
        } else if (c == SH_KEY_DELETE) {
            if (pos < len) {
                memmove(buf + pos, buf + pos + 1, len - pos);
                len--;
            }
        } else if (c == SH_KEY_ARROW_UP) {
            if (hist > 0) {
                hist--;
                snprintf(buf, sizeof(buf), "%s", sh->history[hist]);
                len = pos = strlen(buf);
            }
        } else if (c == SH_KEY_ARROW_DOWN) {
            if (hist + 1 < sh->history_count) {
                hist++;
                snprintf(buf, sizeof(buf), "%s", sh->history[hist]);
                len = pos = strlen(buf);
            } else {
                hist = sh->history_count;
                buf[0] = '\0';
                len = pos = 0;
            }
        } else if (isprint(c) && len + 1 < sizeof(buf)) {
            memmove(buf + pos + 1, buf + pos, len - pos + 1);
            buf[pos++] = (char) c;
            len++;
        }
        sh_refresh_line(prompt, buf, pos);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    snprintf(out, outsz, "%s", buf);
    return 0;
}

static void words_free(struct sh_words *w)
{
    for (int i = 0; i < w->n; i++)
        free(w->v[i]);
    free(w->v);
    w->v = NULL;
    w->n = 0;
    w->cap = 0;
}

static int words_add(struct sh_words *w, const char *s)
{
    char **nv;

    if (w->n == w->cap) {
        int cap = w->cap == 0 ? 8 : w->cap * 2;
        nv = realloc(w->v, (size_t) cap * sizeof(*nv));
        if (!nv)
            return -1;
        w->v = nv;
        w->cap = cap;
    }
    w->v[w->n] = strdup(s);
    if (!w->v[w->n])
        return -1;
    w->n++;
    return 0;
}

static int words_add_fields(struct sh_words *w, const char *s)
{
    const char *p = s;

    while (*p != '\0') {
        const char *start;
        char field[SH_LINE_MAX];
        size_t len;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
        len = (size_t) (p - start);
        if (len >= sizeof(field))
            len = sizeof(field) - 1;
        memcpy(field, start, len);
        field[len] = '\0';
        if (words_add(w, field) != 0)
            return -1;
    }
    return 0;
}

static void trim(char *s)
{
    char *p = s;
    size_t n;

    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                     s[n - 1] == '\t'))
        s[--n] = '\0';
}

static bool is_name_char(int c)
{
    return isalnum(c) || c == '_';
}

static int append_char(char **buf, size_t *len, size_t *cap, int c)
{
    char *nb;

    if (*len + 2 > *cap) {
        size_t ncap = *cap == 0 ? 64 : *cap * 2;
        nb = realloc(*buf, ncap);
        if (!nb)
            return -1;
        *buf = nb;
        *cap = ncap;
    }
    (*buf)[(*len)++] = (char) c;
    (*buf)[*len] = '\0';
    return 0;
}

static int append_str(char **buf, size_t *len, size_t *cap, const char *s)
{
    while (*s != '\0') {
        if (append_char(buf, len, cap, *s++) != 0)
            return -1;
    }
    return 0;
}

/* Resolve a scalar parameter by name: $0, a positional $1..$N, or an
 * environment variable.
 *
 * Returns the value, or NULL if unset (distinct from set-but-empty).
 */
static const char *sh_scalar_param(struct sh *sh, const char *name)
{
    if (name[0] == '0' && name[1] == '\0')
        return sh->arg0 ? sh->arg0 : "sh";
    if (isdigit((unsigned char) name[0])) {
        const char *d = name;
        int idx;

        while (isdigit((unsigned char) *d))
            d++;
        if (*d != '\0') /* digit-prefixed non-name (e.g. 1x): unset */
            return NULL;
        idx = atoi(name);
        if (idx >= 1 && idx <= sh->nparams)
            return sh->params[idx - 1];
        return NULL;
    }
    return getenv(name);
}

/* A POSIX variable name: nonempty, does not start with a digit, all name chars.
 */
static bool is_valid_varname(const char *s)
{
    if (*s == '\0' || isdigit((unsigned char) *s))
        return false;
    for (const char *p = s; *p != '\0'; p++)
        if (!is_name_char((unsigned char) *p))
            return false;
    return true;
}

/* Join $1..$n with a single space into a fresh buffer ("" when empty). */
static char *sh_join_params(struct sh *sh)
{
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    for (int i = 0; i < sh->nparams; i++) {
        if ((i > 0 && append_char(&buf, &len, &cap, ' ') != 0) ||
            append_str(&buf, &len, &cap, sh->params[i]) != 0) {
            free(buf);
            return NULL;
        }
    }
    return buf ? buf : strdup("");
}

/* ${VAR#pat}/${VAR##pat} (prefix) and ${VAR%pat}/${VAR%%pat} (suffix): remove
 * the shortest (single op char) or longest (doubled) fnmatch-matching part.
 * Returns a malloc'd result; the original on no match.
 */
static char *sh_trim(const char *v, const char *pat, int prefix, int longest)
{
    size_t vl = strlen(v);

    if (prefix) {
        char *cand = malloc(vl + 1);

        if (!cand)
            return strdup(v);
        /* shortest: grow the prefix from 0; longest: shrink it from vl. */
        for (size_t k = 0; k <= vl; k++) {
            size_t i = longest ? vl - k : k;

            memcpy(cand, v, i);
            cand[i] = '\0';
            if (fnmatch(pat, cand, 0) == 0) {
                char *r = strdup(v + i);

                free(cand);
                return r;
            }
        }
        free(cand);
        return strdup(v);
    }

    /* suffix: v + j is already NUL-terminated, so no candidate copy is needed.
     * shortest keeps the most (largest j); longest keeps the least.
     */
    for (size_t k = 0; k <= vl; k++) {
        size_t j = longest ? k : vl - k;

        if (fnmatch(pat, v + j, 0) == 0) {
            char *out = malloc(j + 1);

            if (!out)
                return strdup(v);
            memcpy(out, v, j);
            out[j] = '\0';
            return out;
        }
    }
    return strdup(v);
}

/* A ${...} expansion that must fail: print msg, and in a non-interactive shell
 * request exit with a nonzero status (POSIX).
 *
 * Returns an empty expansion.
 */
static char *braced_fail(struct sh *sh, const char *name, const char *msg)
{
    fprintf(stderr, "%s: %s\n", name, msg);
    if (!sh->interactive) {
        sh->exit_requested = true;
        sh->last_status = 1;
    }
    return strdup("");
}

/* set -u: referencing an unset parameter is an error. Print a diagnostic and
 * abort the command being expanded (run_line skips it on seeing expand_abort);
 * a non-interactive shell additionally exits. The caller still expands to the
 * empty string. $@/$* are exempt. The tolerant ${x-w}/${x:-w}/etc. forms never
 * call this; only bare `$x`/`${x}`, `${#x}`, and `${x#pat}` do.
 */
static void nounset_check(struct sh *sh, const char *name, const char *value)
{
    if (value != NULL || !sh->opt_nounset)
        return;
    if (!strcmp(name, "@") || !strcmp(name, "*"))
        return;
    fprintf(stderr, "%s: parameter not set\n", name);
    sh->last_status = 1;
    sh->expand_abort = true;
    if (!sh->interactive)
        sh->exit_requested = true;
}

/* Parse and expand ${ ... }. On entry *pp points just past '{'; on return just
 * past the matching '}'. Supports length (${#name}), ${@}/${*},
 * defaults/assign/ error/alt (${n:-w} ${n-w} ${n:=w} ${n=w} ${n:?w} ${n?w}
 * ${n:+w} ${n+w}), and prefix/suffix trims (${n#pat} ${n##pat} ${n%pat}
 * ${n%%pat}). The word/pattern is taken literally (no nested expansion yet).
 *
 * Returns malloc'd; "" on error.
 */
static char *expand_braced(struct sh *sh, const char **pp)
{
    const char *p = *pp;
    const char *start = p;
    char *content;
    char *name;
    const char *op;
    const char *value;
    size_t clen;
    size_t nlen;
    int colon = 0;
    char *result;

    while (*p != '\0' && *p != '}')
        p++;
    if (*p != '}')
        return strdup("");
    clen = (size_t) (p - start);
    content = malloc(clen + 1);
    if (!content)
        return strdup("");
    memcpy(content, start, clen);
    content[clen] = '\0';
    *pp = p + 1;

    /* ${#name} -> length of the value ($# count is handled in expand_dollar).
     */
    if (content[0] == '#' && content[1] != '\0') {
        char lb[32];

        value = sh_scalar_param(sh, content + 1);
        nounset_check(sh, content + 1, value);
        snprintf(lb, sizeof(lb), "%zu", value ? strlen(value) : (size_t) 0);
        free(content);
        return strdup(lb);
    }
    /* ${@}/${*} -> join positional params with a space (not field-split). */
    if ((content[0] == '@' || content[0] == '*') && content[1] == '\0') {
        result = sh_join_params(sh);
        free(content);
        return result ? result : strdup("");
    }

    /* Split content into name (leading run of name chars) and operator. */
    op = content;
    while (is_name_char((unsigned char) *op))
        op++;
    nlen = (size_t) (op - content);
    name = malloc(nlen + 1);
    if (!name) {
        free(content);
        return strdup("");
    }
    memcpy(name, content, nlen);
    name[nlen] = '\0';
    value = sh_scalar_param(sh, name);
    const char *cur = value ? value : "";

    if (*op == '\0') {
        nounset_check(sh, name, value);
        result = strdup(cur);
    } else if (*op == '#' || *op == '%') {
        int longest = op[1] == *op;
        const char *pat = op + (longest ? 2 : 1);

        nounset_check(sh, name, value);
        result = sh_trim(cur, pat, *op == '#', longest);
    } else {
        int missing;
        const char *word;

        if (*op == ':') {
            colon = 1;
            op++;
        }
        missing = value == NULL || (colon && value[0] == '\0');
        word = op[0] != '\0' ? op + 1 : "";
        switch (*op) {
        case '-':
            result = strdup(missing ? word : cur);
            break;
        case '=':
            if (!missing)
                result = strdup(cur);
            else if (!is_valid_varname(name))
                result = braced_fail(sh, name, "cannot assign in this way");
            else if (setenv(name, word, 1) != 0)
                result = braced_fail(sh, name, "assignment failed");
            else
                result = strdup(word);
            break;
        case '?':
            if (missing)
                result = braced_fail(
                    sh, name, word[0] ? word : "parameter null or not set");
            else
                result = strdup(cur);
            break;
        case '+':
            result = strdup(missing ? "" : word);
            break;
        default:
            result = strdup(cur);
            break;
        }
    }
    free(name);
    free(content);
    return result;
}

static int expand_dollar(struct sh *sh,
                         const char **pp,
                         char **buf,
                         size_t *len,
                         size_t *cap)
{
    const char *p = *pp;
    char name[96];
    size_t n = 0;
    const char *value;
    char tmp[32];
    char *owned = NULL;

    if (*p == '?') {
        snprintf(tmp, sizeof(tmp), "%d", sh->last_status);
        *pp = p + 1;
        return append_str(buf, len, cap, tmp);
    }
    if (*p == '{') {
        p++;
        owned = expand_braced(sh, &p);
        *pp = p;
        if (!owned)
            return -1;
        if (append_str(buf, len, cap, owned) != 0) {
            free(owned);
            return -1;
        }
        free(owned);
        return 0;
    }
    if (*p == '$') { /* $$ -> shell process id */
        snprintf(tmp, sizeof(tmp), "%ld", (long) getpid());
        *pp = p + 1;
        return append_str(buf, len, cap, tmp);
    }
    if (*p == '-') { /* $- -> current single-letter option flags */
        char flags[8];
        size_t k = 0;

        if (sh->interactive)
            flags[k++] = 'i';
        if (sh->opt_errexit)
            flags[k++] = 'e';
        if (sh->opt_nounset)
            flags[k++] = 'u';
        if (sh->opt_xtrace)
            flags[k++] = 'x';
        flags[k] = '\0';
        *pp = p + 1;
        return append_str(buf, len, cap, flags);
    }
    if (*p == '#') { /* $# -> positional parameter count */
        snprintf(tmp, sizeof(tmp), "%d", sh->nparams);
        *pp = p + 1;
        return append_str(buf, len, cap, tmp);
    }
    if (*p == '@' ||
        *p == '*') { /* join $1..$n (unquoted forms not split yet) */
        *pp = p + 1;
        for (int i = 0; i < sh->nparams; i++) {
            if (i > 0 && append_char(buf, len, cap, ' ') != 0)
                return -1;
            if (append_str(buf, len, cap, sh->params[i]) != 0)
                return -1;
        }
        return 0;
    }
    if (isdigit((unsigned char) *p)) { /* $0..$9 (multi-digit needs ${10}) */
        char dn[2] = {*p, '\0'};

        value = sh_scalar_param(sh, dn);
        nounset_check(sh, dn, value);
        *pp = p + 1;
        return append_str(buf, len, cap, value ? value : "");
    }
    if (!is_name_char((unsigned char) *p))
        return append_char(buf, len, cap, '$');
    while (is_name_char((unsigned char) *p) && n + 1 < sizeof(name))
        name[n++] = *p++;
    while (is_name_char((unsigned char) *p))
        p++;
    name[n] = '\0';
    *pp = p;
    value = sh_scalar_param(sh, name);
    nounset_check(sh, name, value);
    return append_str(buf, len, cap, value ? value : "");
}

static int token_add(struct sh_token *tok,
                     int *ntok,
                     enum sh_op op,
                     const char *s)
{
    if (*ntok >= SH_MAX_TOKENS - 1)
        return -1;
    tok[*ntok].op = op;
    tok[*ntok].s = s ? strdup(s) : NULL;
    if (s && !tok[*ntok].s)
        return -1;
    (*ntok)++;
    return 0;
}

static void token_free(struct sh_token *tok, int ntok)
{
    for (int i = 0; i < ntok; i++)
        free(tok[i].s);
}

/* Run `inner` as a command substitution and capture its stdout into *out
 * (malloc'd, trailing newlines stripped). Spawns `sh -c inner` and drains the
 * pipe concurrently, so there is no PIPE_BUF deadlock and output is unbounded.
 * The child inherits environ (all shell variables live there via setenv), which
 * gives correct subshell isolation; shell functions and positional parameters
 * are not visible inside the substitution (revisit once functions land).
 * Returns 0, or -1 on failure (with *out set to "").
 */
static int capture_command(const char *inner, char **out)
{
    int pfd[2];
    posix_spawn_file_actions_t fa;
    char *argv[4];
    pid_t pid;
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    char rb[512];
    ssize_t r;
    int status;

    *out = strdup("");
    if (!*out)
        return -1;
    if (pipe(pfd) != 0)
        return 0;
    if (posix_spawn_file_actions_init(&fa) != EOK) {
        close(pfd[0]);
        close(pfd[1]);
        return 0;
    }

    /* Point the child's stdout at the pipe and drop the pipe fds it inherits.
     * Only redirect/close the write end when it is not already fd 1, mirroring
     * run_external(), so a shell launched with stdout closed is not broken.
     */
    status = EOK;
    if (pfd[1] != STDOUT_FILENO)
        status = posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
    if (status == EOK)
        status = posix_spawn_file_actions_addclose(&fa, pfd[0]);
    if (status == EOK && pfd[1] != STDOUT_FILENO)
        status = posix_spawn_file_actions_addclose(&fa, pfd[1]);
    if (status != EOK) {
        posix_spawn_file_actions_destroy(&fa);
        close(pfd[0]);
        close(pfd[1]);
        return 0;
    }
    argv[0] = "sh";
    argv[1] = "-c";
    argv[2] = (char *) inner;
    argv[3] = NULL;
    status = posix_spawnp(&pid, "sh", &fa, NULL, argv, NULL);
    posix_spawn_file_actions_destroy(&fa);
    close(pfd[1]);
    if (status != EOK) {
        close(pfd[0]);
        return 0;
    }
    while ((r = read(pfd[0], rb, sizeof(rb))) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            if (append_char(&buf, &len, &cap, rb[i]) != 0) {
                free(buf);
                close(pfd[0]);
                (void) waitpid(pid, &status, 0);
                return -1;
            }
        }
    }
    close(pfd[0]);
    (void) waitpid(pid, &status, 0);
    while (len > 0 && buf[len - 1] == '\n')
        buf[--len] = '\0';
    free(*out);
    *out = buf ? buf : strdup("");
    return *out ? 0 : -1;
}

/* Emit a substitution result into the word being built. Double-quoted: append
 * literally. Unquoted: split on IFS whitespace -- the first field joins the
 * current buffer, each later field flushes the buffer as its own token and
 * starts a new one. *produced is set when any field/char is contributed, so the
 * caller can drop a word that is nothing but an empty unquoted substitution.
 *
 * Returns 0, or -1 on allocation failure.
 */
static int emit_sub_result(struct sh_token *tok,
                           int *ntok,
                           char **buf,
                           size_t *len,
                           size_t *cap,
                           int quote,
                           const char *text,
                           int *produced)
{
    const char *p = text;
    int first = 1;

    if (quote) { /* double quotes: a field, even if empty */
        *produced = 1;
        return append_str(buf, len, cap, text);
    }
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (*p == '\0')
            break;
        if (!first) {
            if (token_add(tok, ntok, SH_WORD, *buf ? *buf : "") != 0)
                return -1;
            free(*buf);
            *buf = NULL;
            *len = 0;
            *cap = 0;
        }
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') {
            if (append_char(buf, len, cap, *p++) != 0)
                return -1;
        }
        first = 0;
        *produced = 1;
    }
    return 0;
}

/* Read the body of $( ... ) from *pp (positioned just past "$("), tracking
 * nested parens and quotes; advances *pp past the matching ')'. Malloc'd.
 */
static char *read_paren_sub(const char **pp)
{
    const char *p = *pp;
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    int depth = 1;
    int q = 0;

    while (*p != '\0') {
        char ch = *p;

        if (q) {
            if (q == '"' && ch == '\\' && p[1] != '\0') {
                /* escaped char inside double quotes: keep the backslash and the
                 * escaped char verbatim so a \" does not end the quote early.
                 */
                if (append_char(&buf, &len, &cap, ch) != 0) {
                    free(buf);
                    return NULL;
                }
                p++;
                ch = *p;
            } else if (ch == q) {
                q = 0;
            }
        } else if (ch == '\\' && p[1] != '\0') {
            if (append_char(&buf, &len, &cap, ch) != 0) {
                free(buf);
                return NULL;
            }
            p++;
            ch = *p;
        } else if (ch == '\'' || ch == '"') {
            q = ch;
        } else if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            if (--depth == 0) {
                p++;
                break;
            }
        }
        if (append_char(&buf, &len, &cap, ch) != 0) {
            free(buf);
            return NULL;
        }
        p++;
    }
    *pp = p;
    return buf ? buf : strdup("");
}

/* Read the body of a `...` command substitution from *pp (just past the opening
 * backtick), unescaping \` \\ \$; advances *pp past the closing backtick.
 */
static char *read_backtick(const char **pp)
{
    const char *p = *pp;
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    while (*p != '\0' && *p != '`') {
        if (*p == '\\' && (p[1] == '`' || p[1] == '\\' || p[1] == '$'))
            p++;
        if (append_char(&buf, &len, &cap, *p++) != 0) {
            free(buf);
            return NULL;
        }
    }
    if (*p == '`')
        p++;
    *pp = p;
    return buf ? buf : strdup("");
}

/* Recursive-descent integer evaluator for $(( ... )). Works on long; an unset
 * or non-numeric name is 0. Supports (in precedence order) unary + - ! ~, * /
 * %,
 * binary + -, << >>, relations, == !=, & ^ |, && ||, and parentheses. A leading
 * `$` before a name is accepted (`$x` and `x` are equivalent here). On a syntax
 * error or divide-by-zero *err is set; the partial result is still returned.
 */
/* Cap on '(' nesting in $(( )). Tuning bound: real expressions nest a handful
 * deep, and each level is ~10 recursive-descent frames, so 64 is far above any
 * legitimate use while keeping the worst-case stack small. Revisit if a real
 * expression ever legitimately needs deeper nesting.
 */
#define SH_ARITH_MAX_DEPTH 64

struct arith_ctx {
    const char *p;
    struct sh *sh;
    int err;
    int dead;  /* short-circuited &&/|| branch: parse but suppress div/shift
                  errors
                  */
    int depth; /* current '(' nesting, capped at SH_ARITH_MAX_DEPTH */
};

static long arith_lor(struct arith_ctx *a);

/* Flag an arithmetic error unless we are in a short-circuited (dead) branch. */
static void arith_fail(struct arith_ctx *a)
{
    if (!a->dead)
        a->err = 1;
}

static void arith_skip(struct arith_ctx *a)
{
    while (*a->p == ' ' || *a->p == '\t')
        a->p++;
}

static long arith_primary(struct arith_ctx *a)
{
    long v = 0;

    arith_skip(a);
    if (*a->p == '(') {
        a->p++;
        if (a->depth >= SH_ARITH_MAX_DEPTH) {
            arith_fail(a); /* refuse to recurse deeper; err stops the parse */
            return 0;
        }
        a->depth++;
        v = arith_lor(a);
        a->depth--;
        arith_skip(a);
        if (*a->p == ')')
            a->p++;
        else
            arith_fail(a);
        return v;
    }
    if (*a->p == '$')
        a->p++;
    if (isdigit((unsigned char) *a->p)) {
        char *end;

        v = strtol(a->p, &end, 0);
        a->p = end;
        return v;
    }
    if (isalpha((unsigned char) *a->p) || *a->p == '_') {
        char name[64];
        size_t n = 0;
        const char *val;

        while (is_name_char((unsigned char) *a->p)) {
            if (n < sizeof(name) - 1)
                name[n++] = *a->p; /* consume the whole name, keep first 63 */
            a->p++;
        }
        name[n] = '\0';
        val = sh_scalar_param(a->sh, name);
        return val ? strtol(val, NULL, 0) : 0;
    }
    arith_fail(a);
    return 0;
}

static long arith_unary(struct arith_ctx *a)
{
    arith_skip(a);
    if (*a->p == '-') {
        a->p++;
        return (long) (0UL - (unsigned long) arith_unary(a));
    }
    if (*a->p == '+') {
        a->p++;
        return arith_unary(a);
    }
    if (*a->p == '!') {
        a->p++;
        return !arith_unary(a);
    }
    if (*a->p == '~') {
        a->p++;
        return ~arith_unary(a);
    }
    return arith_primary(a);
}

static long arith_mul(struct arith_ctx *a)
{
    long v = arith_unary(a);

    for (;;) {
        char op;
        long r;

        arith_skip(a);
        op = *a->p;
        if (op != '*' && op != '/' && op != '%')
            break;
        a->p++;
        r = arith_unary(a);
        if (op == '*') {
            v = (long) ((unsigned long) v * (unsigned long) r);
        } else if (r == 0 || (v == LONG_MIN && r == -1)) {
            arith_fail(a); /* divide by zero, or LONG_MIN/-1 overflow */
            v = 0;
        } else if (op == '/') {
            v /= r;
        } else {
            v %= r;
        }
    }
    return v;
}

static long arith_add(struct arith_ctx *a)
{
    long v = arith_mul(a);

    for (;;) {
        arith_skip(a);
        if (*a->p == '+') {
            a->p++;
            v = (long) ((unsigned long) v + (unsigned long) arith_mul(a));
        } else if (*a->p == '-') {
            a->p++;
            v = (long) ((unsigned long) v - (unsigned long) arith_mul(a));
        } else {
            break;
        }
    }
    return v;
}

static long arith_shift(struct arith_ctx *a)
{
    long v = arith_add(a);
    long bits = (long) (sizeof(long) * 8);

    for (;;) {
        long r;

        arith_skip(a);
        if (a->p[0] == '<' && a->p[1] == '<') {
            a->p += 2;
            r = arith_add(a);
            if (r < 0 || r >= bits)
                arith_fail(a);
            else
                v = (long) ((unsigned long) v << r);
        } else if (a->p[0] == '>' && a->p[1] == '>') {
            a->p += 2;
            r = arith_add(a);
            if (r < 0 || r >= bits)
                arith_fail(a);
            else
                v >>= r;
        } else {
            break;
        }
    }
    return v;
}

static long arith_rel(struct arith_ctx *a)
{
    long v = arith_shift(a);

    for (;;) {
        arith_skip(a);
        if (a->p[0] == '<' && a->p[1] == '=') {
            a->p += 2;
            v = v <= arith_shift(a);
        } else if (a->p[0] == '>' && a->p[1] == '=') {
            a->p += 2;
            v = v >= arith_shift(a);
        } else if (a->p[0] == '<' && a->p[1] != '<') {
            a->p++;
            v = v < arith_shift(a);
        } else if (a->p[0] == '>' && a->p[1] != '>') {
            a->p++;
            v = v > arith_shift(a);
        } else {
            break;
        }
    }
    return v;
}

static long arith_eq(struct arith_ctx *a)
{
    long v = arith_rel(a);

    for (;;) {
        arith_skip(a);
        if (a->p[0] == '=' && a->p[1] == '=') {
            a->p += 2;
            v = v == arith_rel(a);
        } else if (a->p[0] == '!' && a->p[1] == '=') {
            a->p += 2;
            v = v != arith_rel(a);
        } else {
            break;
        }
    }
    return v;
}

static long arith_band(struct arith_ctx *a)
{
    long v = arith_eq(a);

    for (;;) {
        arith_skip(a);
        if (a->p[0] == '&' && a->p[1] != '&') {
            a->p++;
            v &= arith_eq(a);
        } else {
            break;
        }
    }
    return v;
}

static long arith_bxor(struct arith_ctx *a)
{
    long v = arith_band(a);

    for (;;) {
        arith_skip(a);
        if (*a->p == '^') {
            a->p++;
            v ^= arith_band(a);
        } else {
            break;
        }
    }
    return v;
}

static long arith_bor(struct arith_ctx *a)
{
    long v = arith_bxor(a);

    for (;;) {
        arith_skip(a);
        if (a->p[0] == '|' && a->p[1] != '|') {
            a->p++;
            v |= arith_bxor(a);
        } else {
            break;
        }
    }
    return v;
}

static long arith_land(struct arith_ctx *a)
{
    long v = arith_bor(a);

    for (;;) {
        arith_skip(a);
        if (a->p[0] == '&' && a->p[1] == '&') {
            int save = a->dead;
            long r;

            a->p += 2;
            if (!v) /* short-circuit: parse the RHS but suppress its errors */
                a->dead = 1;
            r = arith_bor(a);
            a->dead = save;
            v = v && r;
        } else {
            break;
        }
    }
    return v;
}

static long arith_lor(struct arith_ctx *a)
{
    long v = arith_land(a);

    for (;;) {
        arith_skip(a);
        if (a->p[0] == '|' && a->p[1] == '|') {
            int save = a->dead;
            long r;

            a->p += 2;
            if (v) /* short-circuit: parse the RHS but suppress its errors */
                a->dead = 1;
            r = arith_land(a);
            a->dead = save;
            v = v || r;
        } else {
            break;
        }
    }
    return v;
}

/* Read the body of $(( ... )) from *pp (positioned just past "$(("), balancing
 * inner parens. Advances *pp past the closing "))" and sets *closed=1; on a
 * malformed close (single ) or EOF) sets *closed=0 and consumes a stray ) so
 * the caller does not loop. Malloc'd body.
 */
static char *read_arith(const char **pp, int *closed)
{
    const char *p = *pp;
    const char *start = p;
    int depth = 0;
    char *r;
    size_t n;

    while (*p != '\0') {
        if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            if (depth == 0)
                break;
            depth--;
        }
        p++;
    }
    n = (size_t) (p - start);
    r = malloc(n + 1);
    if (r) {
        memcpy(r, start, n);
        r[n] = '\0';
    }
    if (*p == ')' && p[1] == ')') {
        *closed = 1;
        p += 2;
    } else {
        *closed = 0;
        if (*p == ')')
            p++;
    }
    *pp = p;
    return r;
}

static int glob_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* Expand a pathname pattern against the filesystem, appending sorted matches to
 * *out.
 *
 * Returns the number of matches (0 = none: the caller keeps the literal word,
 * per POSIX), or -1 on allocation failure. Only the final path component may be
 * a pattern (e.g. a leading `dir/` then a wildcard); a leading dot is matched
 * only when the pattern itself starts with a dot.
 */
static int glob_word(const char *pat, struct sh_words *out)
{
    char prefix[PATH_MAX]; /* dir part with trailing slash, prepended to hits */
    char diropen[PATH_MAX]; /* dir to opendir(), without a trailing slash */
    const char *slash = strrchr(pat, '/');
    const char *filepat;
    DIR *dp;
    struct dirent *de;
    int start = out->n;

    if (slash) {
        size_t pl = (size_t) (slash - pat) + 1; /* include the slash */

        if (pl >= sizeof(prefix))
            return 0;
        memcpy(prefix, pat, pl);
        prefix[pl] = '\0';
        filepat = slash + 1;
        if (pl == 1) {
            diropen[0] = '/'; /* pattern like /foo -> open "/" */
            diropen[1] = '\0';
        } else {
            memcpy(diropen, pat, pl - 1); /* drop the trailing slash */
            diropen[pl - 1] = '\0';
        }
    } else {
        prefix[0] = '\0';
        filepat = pat;
        diropen[0] = '.';
        diropen[1] = '\0';
    }
    /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
    dp = opendir(diropen);
    if (!dp)
        return 0;
    while ((de = readdir(dp)) != NULL) {
        char full[PATH_MAX];

        if (de->d_name[0] == '.' && filepat[0] != '.')
            continue;
        if (fnmatch(filepat, de->d_name, 0) != 0)
            continue;
        if (snprintf(full, sizeof(full), "%s%s", prefix, de->d_name) >=
            (int) sizeof(full))
            continue;
        if (words_add(out, full) != 0) {
            closedir(dp);
            return -1;
        }
    }
    closedir(dp);
    if (out->n - start > 1)
        qsort(out->v + start, (size_t) (out->n - start), sizeof(*out->v),
              glob_cmp);
    return out->n - start;
}

static int tokenize(struct sh *sh, const char *line, struct sh_token *tok)
{
    int ntok = 0;
    const char *p = line;

    while (*p != '\0') {
        char *buf = NULL;
        size_t len = 0;
        size_t cap = 0;
        int quote = 0;

        /* Whether this word has any literal/quoted content or a non-empty
         * expansion. A word that is only an empty unquoted substitution
         * contributes no field (POSIX), so it is not emitted.
         */
        int word_has_content = 0;
        int word_may_glob = 0;        /* has an unquoted * ? [ metacharacter */
        int word_has_quoted_glob = 0; /* also has a quoted/escaped one */
        int at_word_start = 1;        /* still at the unquoted word start */

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#')
            break;
        if (*p == ';') {
            if (token_add(tok, &ntok, SH_SEMI, NULL) != 0)
                goto fail;
            p++;
            continue;
        }
        if (*p == '&') {
            if (p[1] == '&') {
                if (token_add(tok, &ntok, SH_AND, NULL) != 0)
                    goto fail;
                p += 2;
            } else {
                if (token_add(tok, &ntok, SH_BG, NULL) != 0)
                    goto fail;
                p++;
            }
            continue;
        }
        if (*p == '|') {
            if (p[1] == '|') {
                if (token_add(tok, &ntok, SH_OR, NULL) != 0)
                    goto fail;
                p += 2;
            } else {
                if (token_add(tok, &ntok, SH_PIPE, NULL) != 0)
                    goto fail;
                p++;
            }
            continue;
        }
        if (*p == '<') {
            if (token_add(tok, &ntok, SH_LT, NULL) != 0)
                goto fail;
            p++;
            continue;
        }
        if (*p == '2' && p[1] == '>') {
            if (p[2] == '&') {
                char fdword[16];
                size_t n = 1;

                fdword[0] = '&';
                p += 3;
                while (isdigit((unsigned char) *p) && n + 1 < sizeof(fdword))
                    fdword[n++] = *p++;
                fdword[n] = '\0';
                if (token_add(tok, &ntok, SH_ERRGT, NULL) != 0 ||
                    token_add(tok, &ntok, SH_WORD, fdword) != 0)
                    goto fail;
                continue;
            }
            if (token_add(tok, &ntok, p[2] == '>' ? SH_ERRGTGT : SH_ERRGT,
                          NULL) != 0)
                goto fail;
            p += p[2] == '>' ? 3 : 2;
            continue;
        }
        if (*p == '>') {
            if (token_add(tok, &ntok, p[1] == '>' ? SH_GTGT : SH_GT, NULL) != 0)
                goto fail;
            p += p[1] == '>' ? 2 : 1;
            continue;
        }
        while (*p != '\0') {
            if (quote == 0 &&
                (*p == ' ' || *p == '\t' || *p == ';' || *p == '&' ||
                 *p == '|' || *p == '<' || *p == '>' || *p == '#'))
                break;

            /* Tilde only at the true, unquoted start of the word: standing
             * alone or before a slash. HOME unset leaves ~ literal.
             */
            if (at_word_start && quote == 0 && *p == '~' &&
                (p[1] == '\0' || p[1] == '/' ||
                 strchr(" \t;&|<>", p[1]) != NULL)) {
                const char *home = getenv("HOME");

                at_word_start = 0;
                if (home) {
                    if (append_str(&buf, &len, &cap, home) != 0) {
                        free(buf);
                        goto fail;
                    }
                    word_has_content = 1;
                    p++;
                    continue;
                }
            }
            at_word_start = 0;
            if (*p == '\'' && quote != '"') {
                quote = quote == '\'' ? 0 : '\'';
                word_has_content = 1;
                p++;
                continue;
            }
            if (*p == '"' && quote != '\'') {
                quote = quote == '"' ? 0 : '"';
                word_has_content = 1;
                p++;
                continue;
            }
            if (*p == '\\' && p[1] != '\0') {
                p++;
                if (*p == '*' || *p == '?' || *p == '[')
                    word_has_quoted_glob = 1; /* escaped: keep it literal */
                word_has_content = 1;
                if (append_char(&buf, &len, &cap, *p++) != 0) {
                    free(buf);
                    goto fail;
                }
                continue;
            }
            if (*p == '$' && p[1] == '(' && p[2] == '(' && quote != '\'') {
                /* arithmetic expansion $(( expr )) */
                char *expr;
                char nb[32];
                struct arith_ctx a;
                long val;
                int closed;

                p += 3;
                expr = read_arith(&p, &closed);
                if (!expr) {
                    free(buf);
                    goto fail;
                }
                a.p = expr;
                a.sh = sh;
                a.err = 0;
                a.dead = 0;
                a.depth = 0;
                val = arith_lor(&a);
                arith_skip(&a);
                if (!closed || a.err || *a.p != '\0') {
                    fprintf(stderr, "sh: arithmetic error: %s\n", expr);
                    val = 0;
                }
                free(expr);
                snprintf(nb, sizeof(nb), "%ld", val);
                word_has_content = 1;
                if (append_str(&buf, &len, &cap, nb) != 0) {
                    free(buf);
                    goto fail;
                }
                continue;
            }
            if (((*p == '$' && p[1] == '(') || *p == '`') && quote != '\'') {
                char *inner;
                char *result;

                if (*p == '`') {
                    p++;
                    inner = read_backtick(&p);
                } else {
                    p += 2;
                    inner = read_paren_sub(&p);
                }
                if (!inner) {
                    free(buf);
                    goto fail;
                }
                if (capture_command(inner, &result) != 0) {
                    free(inner);
                    free(result);
                    free(buf);
                    goto fail;
                }
                free(inner);
                if (emit_sub_result(tok, &ntok, &buf, &len, &cap, quote, result,
                                    &word_has_content) != 0) {
                    free(result);
                    free(buf);
                    goto fail;
                }
                free(result);
                continue;
            }
            if (*p == '$' && quote != '\'') {
                p++;

                /* $NAME/${...}/$? keep the pre-existing behavior of yielding a
                 * word even when empty, so only substitutions are dropped.
                 */
                word_has_content = 1;
                if (expand_dollar(sh, &p, &buf, &len, &cap) != 0) {
                    free(buf);
                    goto fail;
                }
                continue;
            }

            /* An unquoted glob metacharacter marks the word for pathname
             * expansion; a quoted/escaped one must stay literal, so a word that
             * mixes both is not globbed (its quoted metachar would go active).
             */
            if (*p == '*' || *p == '?' || *p == '[') {
                if (quote == 0)
                    word_may_glob = 1;
                else
                    word_has_quoted_glob = 1;
            }
            word_has_content = 1;
            if (append_char(&buf, &len, &cap, *p++) != 0) {
                free(buf);
                goto fail;
            }
        }
        if (word_may_glob && !word_has_quoted_glob && buf && buf[0] != '\0') {
            struct sh_words g = {0};
            int m = glob_word(buf, &g);

            if (m < 0) {
                words_free(&g);
                free(buf);
                goto fail;
            }
            if (m > 0) {
                for (int i = 0; i < g.n; i++) {
                    if (token_add(tok, &ntok, SH_WORD, g.v[i]) != 0) {
                        words_free(&g);
                        free(buf);
                        goto fail;
                    }
                }
                words_free(&g);
                free(buf);
                continue; /* word replaced by its matches */
            }
            words_free(&g); /* no match: fall through to the literal word */
        }
        if (word_has_content || (buf && buf[0] != '\0')) {
            if (!buf)
                buf = strdup("");
            if (!buf)
                goto fail;
            if (token_add(tok, &ntok, SH_WORD, buf) != 0) {
                free(buf);
                goto fail;
            }
        }
        free(buf);
    }
    return ntok;

fail:
    token_free(tok, ntok);
    return -1;
}

static bool is_assignment(const char *s)
{
    const char *eq;

    if (!s || !isalpha((unsigned char) s[0]))
        return false;
    eq = strchr(s, '=');
    if (!eq)
        return false;
    for (const char *p = s; p < eq; p++) {
        if (!is_name_char((unsigned char) *p))
            return false;
    }
    return true;
}

static void sh_unsetenv(const char *name)
{
    char **p;
    char **q;
    size_t len;

    if (!name || *name == '\0' || strchr(name, '=') || !sh_environ())
        return;

    len = strlen(name);
    for (p = q = sh_environ(); *p; p++) {
        if (strncmp(*p, name, len) == 0 && (*p)[len] == '=')
            continue;
        *q++ = *p;
    }
    *q = NULL;
}

static int builtin_test(int argc, char **argv)
{
    int off = 0;
    int end = argc;
    int n;

    if (argc > 0 && !strcmp(argv[0], "test"))
        off = 1;
    if (argc > 0 && !strcmp(argv[0], "[")) {
        off = 1;
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fputs("[: missing ]\n", stderr);
            return 2;
        }
        end--;
    }
    if (argc > 0 && !strcmp(argv[0], "[[")) {
        off = 1;
        if (argc < 2 || strcmp(argv[argc - 1], "]]") != 0) {
            fputs("[[: missing ]]\n", stderr);
            return 2;
        }
        end--;
    }
    n = end - off;
    if (n >= 2 && !strcmp(argv[off], "-n"))
        return argv[off + 1][0] != '\0' ? 0 : 1;
    if (n >= 2 && !strcmp(argv[off], "-z"))
        return argv[off + 1][0] == '\0' ? 0 : 1;
    if (n >= 2 && !strcmp(argv[off], "-x"))
        return access(argv[off + 1], X_OK) == 0 ? 0 : 1;
    if (n >= 2 && !strcmp(argv[off], "-e"))
        return access(argv[off + 1], F_OK) == 0 ? 0 : 1;
    if (n >= 3 && !strcmp(argv[off + 1], "="))
        return !strcmp(argv[off], argv[off + 2]) ? 0 : 1;
    if (n >= 3 && !strcmp(argv[off + 1], "!="))
        return strcmp(argv[off], argv[off + 2]) != 0 ? 0 : 1;
    return n > 0 && argv[off][0] != '\0' ? 0 : 1;
}

/* read [-r] var...: read one line and split it on IFS whitespace across the
 * named variables; the last variable receives the remainder (with leading and
 * trailing whitespace trimmed).
 *
 * Returns 1 at end of input with nothing read, so `while read x; do ...; done`
 * terminates. A final line that has data but no trailing newline is returned
 * with status 0 (so it is not silently skipped, unlike bash). -r is accepted;
 * this shell always reads raw, i.e. does not treat a trailing backslash as line
 * continuation.
 */
static int builtin_read(char **argv, int in_fd)
{
    char buf[SH_LINE_MAX];
    size_t n = 0;
    int varidx = 1;
    int nvars = 0;
    int got = 0;
    const char *p;

    if (argv[1] && !strcmp(argv[1], "-r"))
        varidx = 2;
    if (!argv[varidx])
        return 1;
    for (;;) {
        char c;
        ssize_t r = read(in_fd, &c, 1);

        if (r < 0) {
            if (errno == EINTR)
                continue; /* a caught signal, not end of input */
            break;
        }
        if (r == 0)
            break; /* end of input */
        got = 1;
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        if (n + 1 < sizeof(buf))
            buf[n++] =
                c; /* past the buffer: drop, but keep draining the line */
    }
    buf[n] = '\0';
    if (!got)
        return 1; /* end of input, nothing read */

    while (argv[varidx + nvars])
        nvars++;
    p = buf;
    for (int vi = 0; vi < nvars; vi++) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (vi == nvars - 1) {
            char rest[SH_LINE_MAX];
            size_t rl;

            snprintf(rest, sizeof(rest), "%s", p);
            rl = strlen(rest);
            while (rl > 0 && (rest[rl - 1] == ' ' || rest[rl - 1] == '\t'))
                rest[--rl] = '\0';
            if (setenv(argv[varidx + vi], rest, 1) != 0)
                return 1;
        } else {
            char field[SH_LINE_MAX];
            size_t fl = 0;

            while (*p != '\0' && *p != ' ' && *p != '\t' &&
                   fl + 1 < sizeof(field))
                field[fl++] = *p++;
            field[fl] = '\0';
            if (setenv(argv[varidx + vi], field, 1) != 0)
                return 1;
        }
    }
    return 0;
}

static int builtin_waitfor(int argc, char **argv)
{
    unsigned seconds = 10;
    unsigned polls;
    char *end;

    if (argc < 2 || argc > 3) {
        fputs("usage: waitfor PATH [SECONDS]\n", stderr);
        return 2;
    }
    if (argc == 3) {
        unsigned long v = strtoul(argv[2], &end, 10);
        if (*end != '\0' || v > 3600) {
            fprintf(stderr, "waitfor: invalid timeout '%s'\n", argv[2]);
            return 2;
        }
        seconds = (unsigned) v;
    }

    /* Poll once per 10 ms of real time so SECONDS is an actual wall-clock
     * bound. The old loop busy-spun access() with no delay, making "seconds" a
     * meaningless spin count: CPU-hot and far shorter than requested, which let
     * init race past a device whose I/O service had not finished registering.
     */
    polls = seconds * 100u + 1u;
    for (;;) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};

        if (access(argv[1], F_OK) == 0)
            return 0;
        if (polls-- == 0)
            break;
        nanosleep(&ts, NULL);
    }

    fprintf(stderr, "waitfor: %s: %s\n", argv[1], strerror(errno));
    return 1;
}

/* Replace the positional parameters ($1..$n) with a fresh copy of args[0..n-1].
 * On allocation failure the old parameters are left intact.
 */
static int sh_set_params(struct sh *sh, char **args, int n)
{
    char **np = NULL;

    if (n > 0) {
        np = calloc((size_t) n, sizeof(*np));
        if (!np)
            return -1;
        for (int i = 0; i < n; i++) {
            np[i] = strdup(args[i] ? args[i] : "");
            if (!np[i]) {
                while (i-- > 0)
                    free(np[i]);
                free(np);
                return -1;
            }
        }
    }
    for (int i = 0; i < sh->nparams; i++)
        free(sh->params[i]);
    free(sh->params);
    sh->params = np;
    sh->nparams = n;
    return 0;
}

static bool is_builtin(const char *name)
{
    for (int i = 0; sh_builtin_names[i]; i++)
        if (!strcmp(sh_builtin_names[i], name))
            return true;
    return false;
}

/* True if `p` is an executable regular file. access(X_OK) alone also accepts a
 * searchable directory (and other non-regular nodes), which exec cannot run.
 */
static bool path_is_exec(const char *p)
{
    struct stat st;

    return access(p, X_OK) == 0 && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Search $PATH for an executable `name`, writing its full path to out on
 * success. A name containing '/' is checked directly. A candidate that would
 * not fit in out is skipped rather than matched truncated.
 *
 * Returns true if found.
 */
static bool find_in_path(const char *name, char *out, size_t outsz)
{
    const char *path;
    const char *p;

    if (!name || !*name)
        return false;
    if (strchr(name, '/'))
        return path_is_exec(name) &&
               (size_t) snprintf(out, outsz, "%s", name) < outsz;
    path = getenv("PATH");
    if (!path || !*path)
        path = "/bin:/sbin:/usr/bin:/usr/sbin";
    for (p = path;;) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t) (colon - p) : strlen(p);
        int nprint;

        /* An empty entry (including a leading or trailing colon) means cwd. */
        if (dlen == 0)
            nprint = snprintf(out, outsz, "%s", name);
        else
            nprint = snprintf(out, outsz, "%.*s/%s", (int) dlen, p, name);
        if (nprint > 0 && (size_t) nprint < outsz && path_is_exec(out))
            return true;
        if (!colon)
            return false;
        p = colon + 1;
    }
}

/* Describe how `name` resolves, for `command`/`type`. mode 'v' prints the terse
 * form (the name for a builtin/function, the path for an executable); any other
 * mode prints the wordy "NAME is ..." form. A builtin is reported before a
 * function because command_from_tokens dispatches builtins first (so a function
 * cannot shadow one here).
 *
 * Returns 0 if resolved, 1 if not.
 */
static int sh_describe(struct sh *sh, const char *name, char mode)
{
    char path[PATH_MAX];

    if (is_builtin(name)) {
        if (mode == 'v')
            puts(name);
        else
            printf("%s is a shell builtin\n", name);
        return 0;
    }
    if (sh_find_func(sh, name)) {
        if (mode == 'v')
            puts(name);
        else
            printf("%s is a function\n", name);
        return 0;
    }
    if (find_in_path(name, path, sizeof(path))) {
        if (mode == 'v')
            puts(path);
        else
            printf("%s is %s\n", name, path);
        return 0;
    }
    if (mode != 'v')
        fprintf(stderr, "%s: not found\n", name);
    return 1;
}

/* ---- interactive job control ----
 *
 * An interactive shell on its controlling terminal keeps itself in a process
 * group of its own and runs each pipeline in a group of its own, which it makes
 * the terminal's foreground group while the pipeline runs. ^C and ^Z then reach
 * the job and not the shell, which ignores SIGINT, SIGQUIT, SIGTSTP, SIGTTIN
 * and SIGTTOU. A stopped job stays in the table for fg/bg; a background job
 * stays there until it exits.
 */
#define SH_JOBS 16

struct sh_job {
    pid_t pgid;
    pid_t pid; /* the process the shell waits for (the last of a pipeline) */
    bool stopped;
    char cmd[48];
};

static struct sh_job sh_jobs[SH_JOBS];
static bool sh_jc;          /* the shell owns the terminal's foreground */
static pid_t sh_jc_pgid;    /* group of the pipeline being started, else 0 */
static pid_t sh_shell_pgid; /* the shell's own group */
static char sh_jc_cmd[48];  /* text of the command being started */

static struct sh_job *sh_job_find(pid_t pid)
{
    for (int i = 0; i < SH_JOBS; i++)
        if (sh_jobs[i].pid == pid)
            return &sh_jobs[i];
    return NULL;
}

static struct sh_job *sh_job_add(pid_t pgid,
                                 pid_t pid,
                                 const char *cmd,
                                 bool stopped)
{
    struct sh_job *j = sh_job_find(pid);

    if (!j)
        j = sh_job_find(0);
    if (!j)
        return NULL;
    j->pgid = pgid;
    j->pid = pid;
    j->stopped = stopped;
    snprintf(j->cmd, sizeof j->cmd, "%s", cmd);
    return j;
}

static int sh_job_number(const struct sh_job *j)
{
    return (int) (j - sh_jobs) + 1;
}

static void sh_job_report(const struct sh_job *j, const char *what)
{
    printf("[%d]+  %s\t%s\n", sh_job_number(j), what, j->cmd);
}

/* Give the terminal back to the shell once a foreground job has finished or
 * stopped.
 */
static void sh_take_terminal(void)
{
    if (sh_jc)
        (void) tcsetpgrp(STDIN_FILENO, sh_shell_pgid);
}

/* Wait for a foreground job's last process. Returns its exit status as the shell
 * reports it; *stopped is set when the job stopped instead (^Z or a stop
 * signal), in which case it is in the job table.
 */
static int sh_wait_fg(pid_t pid, pid_t pgid, const char *cmd, bool *stopped)
{
    int st = 0;

    *stopped = false;
    for (;;) {
        pid_t r = waitpid(pid, &st, sh_jc ? WUNTRACED : 0);
        struct sh_job *j;

        if (r != pid)
            return 128;
        if (!WIFSTOPPED(st)) {
            if ((j = sh_job_find(pid)))
                j->pid = 0;
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        }
        /* A job that read or wrote before it was handed the terminal was
         * stopped by SIGTTIN/SIGTTOU; it is the foreground group now, so let it
         * go on.
         */
        if ((WSTOPSIG(st) == SIGTTIN || WSTOPSIG(st) == SIGTTOU) &&
            tcgetpgrp(STDIN_FILENO) == pgid) {
            (void) kill(-pgid, SIGCONT);
            continue;
        }
        if ((j = sh_job_add(pgid, pid, cmd, true)))
            sh_job_report(j, "Stopped");
        *stopped = true;
        return 128 + WSTOPSIG(st);
    }
}

/* Pick the job a fg/bg/jobs argument names: "%N" or "N"; none means the most
 * recent one. Returns NULL (after a message) when there is no such job.
 */
static struct sh_job *sh_job_arg(const char *name, const char *arg)
{
    int n = 0;

    if (arg) {
        n = atoi(arg[0] == '%' ? arg + 1 : arg);
    } else {
        for (int i = 0; i < SH_JOBS; i++)
            if (sh_jobs[i].pid)
                n = i + 1;
    }
    if (n < 1 || n > SH_JOBS || !sh_jobs[n - 1].pid) {
        fprintf(stderr, "%s: no such job\n", name);
        return NULL;
    }
    return &sh_jobs[n - 1];
}

static int builtin_jobs(void)
{
    for (int i = 0; i < SH_JOBS; i++)
        if (sh_jobs[i].pid)
            sh_job_report(&sh_jobs[i], sh_jobs[i].stopped ? "Stopped" : "Running");
    return 0;
}

static int builtin_fg_bg(bool fg, char **argv)
{
    struct sh_job *j;
    char cmd[sizeof j->cmd];
    pid_t pid, pgid;
    bool stopped;

    if (!sh_jc) {
        fprintf(stderr, "%s: no job control\n", argv[0]);
        return 1;
    }
    if (!(j = sh_job_arg(argv[0], argv[1])))
        return 1;
    pid = j->pid;
    pgid = j->pgid;
    snprintf(cmd, sizeof cmd, "%s", j->cmd);
    j->stopped = false;
    if (!fg) {
        printf("[%d]+ %s &\n", sh_job_number(j), cmd);
        return kill(-pgid, SIGCONT) == 0 ? 0 : 1;
    }
    puts(cmd);
    (void) tcsetpgrp(STDIN_FILENO, pgid);
    (void) kill(-pgid, SIGCONT);
    {
        int rc = sh_wait_fg(pid, pgid, cmd, &stopped);

        sh_take_terminal();
        return rc;
    }
}

static int run_builtin(struct sh *sh,
                       char **argv,
                       int argc,
                       int in_fd,
                       int *handled)
{
    *handled = 1;
    if (argc == 0)
        return 0;
    if (!strcmp(argv[0], "cd")) {
        const char *dir = argv[1] ? argv[1] : getenv("HOME");
        if (!dir)
            dir = "/";
        if (chdir(dir) != 0) {
            fprintf(stderr, "cd: %s: %s\n", dir, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (!strcmp(argv[0], "exit")) {
        sh->exit_requested = true;
        return argv[1] ? atoi(argv[1]) : sh->last_status;
    }
    if (!strcmp(argv[0], "export")) {
        if (argc == 1) {
            char **envp = sh_environ();

            for (int i = 0; envp && envp[i]; i++)
                puts(envp[i]);
            return 0;
        }
        for (int i = 1; argv[i]; i++) {
            if (is_assignment(argv[i])) {
                char *tmp = strdup(argv[i]);
                char *eq;
                if (!tmp)
                    return 1;
                eq = strchr(tmp, '=');
                *eq++ = '\0';
                if (setenv(tmp, eq, 1) != 0) {
                    free(tmp);
                    return 1;
                }
                free(tmp);
            }
        }
        return 0;
    }
    if (!strcmp(argv[0], "unset")) {
        for (int i = 1; argv[i]; i++)
            sh_unsetenv(argv[i]);
        return 0;
    }
    if (!strcmp(argv[0], "pwd")) {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            return 1;
        puts(cwd);
        return 0;
    }
    if (!strcmp(argv[0], "type")) {
        int rc = 0;

        for (int i = 1; i < argc; i++)
            if (sh_describe(sh, argv[i], 't') != 0)
                rc = 1;
        return rc;
    }
    if (!strcmp(argv[0], "command")) {
        /* command -v/-V NAME: report how NAME resolves. The run form (command
         * NAME [args], which bypasses shell functions) is handled earlier in
         * command_from_tokens, so it does not reach here.
         */
        if (argc == 1) /* bare `command`: nothing to do */
            return 0;
        if (argc >= 3 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "-V"))) {
            int rc = 0;

            for (int i = 2; i < argc; i++)
                if (sh_describe(sh, argv[i], argv[1][1]) != 0)
                    rc = 1;
            return rc;
        }
        fprintf(stderr,
                "command: usage: command -v/-V NAME | command NAME [args]\n");
        return 2;
    }
    if (!strcmp(argv[0], "echo")) {
        int start = 1;
        int nl = 1;
        if (argv[1] && !strcmp(argv[1], "-n")) {
            nl = 0;
            start = 2;
        }
        for (int i = start; argv[i]; i++)
            printf("%s%s", i > start ? " " : "", argv[i]);
        if (nl)
            putchar('\n');
        return 0;
    }
    if (!strcmp(argv[0], "help")) {
        puts(
            "builtins: : cd exit export unset set shift pwd echo read source "
            "exec test waitfor help");
        puts(
            "editing: arrows/home/end, backspace/delete, Ctrl-A/E/U/K/L/D, "
            "Tab");
        return 0;
    }
    if (!strcmp(argv[0], "read"))
        return builtin_read(argv, in_fd);
    if (!strcmp(argv[0], "waitfor"))
        return builtin_waitfor(argc, argv);
    if (!strcmp(argv[0], "jobs"))
        return builtin_jobs();
    if (!strcmp(argv[0], "fg") || !strcmp(argv[0], "bg"))
        return builtin_fg_bg(argv[0][0] == 'f', argv);
    if (!strcmp(argv[0], ".") || !strcmp(argv[0], "source"))
        return argv[1] ? run_file(sh, argv[1]) : 1;
    if (!strcmp(argv[0], "true"))
        return 0;
    if (!strcmp(argv[0], "false"))
        return 1;
    if (!strcmp(argv[0], "eval")) {
        char *joined = NULL;
        size_t len = 0;
        size_t cap = 0;
        int rc;

        for (int i = 1; i < argc; i++) {
            if ((i > 1 && append_char(&joined, &len, &cap, ' ') != 0) ||
                append_str(&joined, &len, &cap, argv[i]) != 0) {
                free(joined);
                return 1;
            }
        }
        rc = joined ? run_line(sh, joined) : 0;
        free(joined);
        return rc;
    }
    if (!strcmp(argv[0], "[[") || !strcmp(argv[0], "test") ||
        !strcmp(argv[0], "["))
        return builtin_test(argc, argv);
    if (!strcmp(argv[0], ":"))
        return 0;
    if (!strcmp(argv[0], "set")) {
        int i = 1;
        bool sep = false;

        while (i < argc && (argv[i][0] == '-' || argv[i][0] == '+') &&
               argv[i][1] != '\0') {
            bool on = argv[i][0] == '-';

            if (!strcmp(argv[i] + 1, "-")) { /* "--" ends option parsing */
                sep = true;
                i++;
                break;
            }
            for (const char *f = argv[i] + 1; *f != '\0'; f++) {
                switch (*f) {
                case 'e':
                    sh->opt_errexit = on;
                    break;
                case 'u':
                    sh->opt_nounset = on;
                    break;
                case 'x':
                    sh->opt_xtrace = on;
                    break;
                default:
                    break;
                }
            }
            i++;
        }
        if (i < argc || sep)
            sh_set_params(sh, argv + i, argc - i);
        else if (argc == 1) {
            char **envp = sh_environ();

            for (int k = 0; envp && envp[k]; k++)
                puts(envp[k]);
        }
        return 0;
    }
    if (!strcmp(argv[0], "shift")) {
        int n = argv[1] ? atoi(argv[1]) : 1;

        if (n < 0 || n > sh->nparams)
            return 1;
        sh_set_params(sh, sh->params + n, sh->nparams - n);
        return 0;
    }
    if (!strcmp(argv[0], "break") || !strcmp(argv[0], "continue")) {
        int n = 1;

        if (argv[1]) {
            char *end;
            long v = strtol(argv[1], &end, 10);

            if (end == argv[1] || *end != '\0' || v < 1) {
                fprintf(stderr, "%s: %s: numeric argument required\n", argv[0],
                        argv[1]);
                return 2;
            }
            n = (int) v;
        }
        if (sh->loop_depth == 0) /* outside a loop: a no-op */
            return 0;
        if (n > sh->loop_depth)
            n = sh->loop_depth;
        if (argv[0][0] == 'b')
            sh->loop_break = n;
        else
            sh->loop_continue = n;
        return 0;
    }
    if (!strcmp(argv[0], "return")) {
        int status = argv[1] ? atoi(argv[1]) : sh->last_status;

        if (sh->fn_depth == 0) /* outside a function: a no-op */
            return status;
        sh->return_requested = true;
        sh->return_status = status;
        return status;
    }
    *handled = 0;
    return 0;
}

static void close_if_needed(int fd)
{
    if (fd > 2)
        close(fd);
}

static void sh_reap_zombies(void)
{
    int st;
    pid_t pid;

    while ((pid = waitpid(-1, &st, sh_jc ? WNOHANG | WUNTRACED : WNOHANG)) > 0) {
        struct sh_job *j = sh_job_find(pid);

        if (!j)
            continue;
        if (WIFSTOPPED(st)) {
            j->stopped = true;
            sh_job_report(j, "Stopped");
        } else {
            sh_job_report(j, "Done");
            j->pid = 0;
        }
    }
}

/* Spawn argv with the given fds. If pid_out is non-NULL, the pid is returned
 * via *pid_out and the function returns 0 without waiting (used to assemble a
 * pipeline that is waited on collectively). Otherwise, the call blocks on
 * waitpid().
 */
static int run_external(char **argv,
                        int in_fd,
                        int out_fd,
                        int err_fd,
                        int background,
                        pid_t *pid_out)
{
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_t *actions_p = NULL;
    posix_spawnattr_t attr;
    posix_spawnattr_t *attr_p = NULL;
    pid_t pid;
    int status = 0;
    int err;

    err = posix_spawn_file_actions_init(&actions);
    if (err != EOK)
        goto fail;
    actions_p = &actions;
    if (in_fd >= 0 && in_fd != STDIN_FILENO) {
        err = posix_spawn_file_actions_adddup2(&actions, in_fd, STDIN_FILENO);
        if (err != EOK)
            goto fail;
        err = posix_spawn_file_actions_addclose(&actions, in_fd);
        if (err != EOK)
            goto fail;
    }
    if (out_fd >= 0 && out_fd != STDOUT_FILENO) {
        err = posix_spawn_file_actions_adddup2(&actions, out_fd, STDOUT_FILENO);
        if (err != EOK)
            goto fail;
        err = posix_spawn_file_actions_addclose(&actions, out_fd);
        if (err != EOK)
            goto fail;
    }
    if (err_fd >= 0 && err_fd != STDERR_FILENO) {
        err = posix_spawn_file_actions_adddup2(&actions, err_fd, STDERR_FILENO);
        if (err != EOK)
            goto fail;
        err = posix_spawn_file_actions_addclose(&actions, err_fd);
        if (err != EOK)
            goto fail;
    }

    if (sh_jc) {
        /* The first process of a pipeline starts its group; the rest join it. */
        if (posix_spawnattr_init(&attr) == EOK) {
            attr_p = &attr;
            (void) posix_spawnattr_setpgroup(attr_p, sh_jc_pgid);
            (void) posix_spawnattr_setflags(attr_p, POSIX_SPAWN_SETPGROUP);
        }
        snprintf(sh_jc_cmd, sizeof sh_jc_cmd, "%s", argv[0]);
    }
    err = posix_spawnp(&pid, argv[0], actions_p, attr_p, argv, NULL);
    posix_spawn_file_actions_destroy(&actions);
    if (attr_p)
        posix_spawnattr_destroy(attr_p);
    if (err != EOK) {
        fprintf(stderr, "%s: %s\n", argv[0], strerror(err));
        return 127;
    }
    if (sh_jc && !sh_jc_pgid) {
        sh_jc_pgid = pid;
        if (!background) {
            (void) tcsetpgrp(STDIN_FILENO, pid);
            /* It may have read the terminal before it was ours and been
             * stopped by SIGTTIN; it is foreground now, so let it go on.
             */
            (void) kill(-pid, SIGCONT);
        }
    }
    if (background) {
        struct sh_job *j = sh_jc ? sh_job_add(sh_jc_pgid, pid, argv[0], false)
                                 : NULL;

        if (j)
            printf("[%d] %d\n", sh_job_number(j), pid);
        else
            printf("%d\n", pid);
        return 0;
    }
    if (pid_out) {
        *pid_out = pid;
        return 0;
    }
    if (waitpid(pid, &status, 0) < 0)
        return 1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128;

fail:
    if (actions_p)
        posix_spawn_file_actions_destroy(&actions);
    fprintf(stderr, "%s: %s\n", argv[0], strerror(err));
    return 127;
}

static int builtin_exec(char **argv, int in_fd, int out_fd, int err_fd)
{
    if (!argv[1]) {
        if (in_fd >= 0 && in_fd != STDIN_FILENO)
            dup2(in_fd, STDIN_FILENO);
        if (out_fd >= 0 && out_fd != STDOUT_FILENO)
            dup2(out_fd, STDOUT_FILENO);
        if (err_fd >= 0 && err_fd != STDERR_FILENO)
            dup2(err_fd, STDERR_FILENO);
        return 0;
    }
    execvp(argv[1], &argv[1]);
    fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
    return 127;
}

/* Redirect stdio to the in/out/err fds around a builtin call so commands like
 * `echo hi >file` and `read x | builtin` honour redirection.
 *
 * Returns 0 on success. The caller restores via sh_redir_restore().
 */
static int sh_redir_apply(int in_fd, int out_fd, int err_fd, int saved[3])
{
    saved[0] = saved[1] = saved[2] = -1;
    if (in_fd >= 0 && in_fd != STDIN_FILENO) {
        saved[0] = dup(STDIN_FILENO);
        if (saved[0] < 0 || dup2(in_fd, STDIN_FILENO) < 0)
            return -1;
    }
    if (out_fd >= 0 && out_fd != STDOUT_FILENO) {
        fflush(stdout);
        saved[1] = dup(STDOUT_FILENO);
        if (saved[1] < 0 || dup2(out_fd, STDOUT_FILENO) < 0)
            return -1;
    }
    if (err_fd >= 0 && err_fd != STDERR_FILENO) {
        fflush(stderr);
        saved[2] = dup(STDERR_FILENO);
        if (saved[2] < 0 || dup2(err_fd, STDERR_FILENO) < 0)
            return -1;
    }
    return 0;
}

static void sh_redir_restore(int saved[3])
{
    if (saved[1] >= 0) {
        fflush(stdout);
        dup2(saved[1], STDOUT_FILENO);
        close(saved[1]);
    }
    if (saved[2] >= 0) {
        fflush(stderr);
        dup2(saved[2], STDERR_FILENO);
        close(saved[2]);
    }
    if (saved[0] >= 0) {
        dup2(saved[0], STDIN_FILENO);
        close(saved[0]);
    }
}

static int command_from_tokens(struct sh *sh,
                               struct sh_token *tok,
                               int a,
                               int b,
                               int in_fd,
                               int out_fd,
                               int err_fd,
                               int background,
                               pid_t *pid_out)
{
    char *argv[SH_MAX_TOKENS];
    int argc = 0;
    int own_in = -1;
    int own_out = -1;
    int own_err = -1;
    int handled = 0;
    int skip_func = 0;
    int rc;
    const char *open_path = NULL;

    for (int i = a; i < b; i++) {
        if (tok[i].op == SH_WORD) {
            argv[argc++] = tok[i].s;
        } else if ((tok[i].op == SH_LT || tok[i].op == SH_GT ||
                    tok[i].op == SH_GTGT || tok[i].op == SH_ERRGT ||
                    tok[i].op == SH_ERRGTGT) &&
                   i + 1 < b && tok[i + 1].op == SH_WORD) {
            int flags;
            int fd;
            open_path = tok[i + 1].s;
            if (tok[i].op == SH_LT) {
                /* `cmd <a <b` keeps the last input; close the prior open. */
                close_if_needed(own_in);
                own_in = -1;
                /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
                own_in = open(tok[i + 1].s, O_RDONLY);
                if (own_in < 0)
                    goto open_fail;
            } else if ((tok[i].op == SH_ERRGT || tok[i].op == SH_ERRGTGT) &&
                       tok[i + 1].s[0] == '&') {
                int target = atoi(tok[i + 1].s + 1);
                close_if_needed(own_err);
                own_err = -1;
                if (target == STDOUT_FILENO && own_out >= 0)
                    own_err = dup(own_out);
                else
                    own_err = dup(target);
                if (own_err < 0)
                    goto open_fail;
            } else {
                flags = O_WRONLY | O_CREAT |
                        ((tok[i].op == SH_GTGT || tok[i].op == SH_ERRGTGT)
                             ? O_APPEND
                             : O_TRUNC);
                /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
                fd = open(tok[i + 1].s, flags, 0666);
                if (fd < 0)
                    goto open_fail;
                if (tok[i].op == SH_ERRGT || tok[i].op == SH_ERRGTGT) {
                    close_if_needed(own_err);
                    own_err = fd;
                } else {
                    close_if_needed(own_out);
                    own_out = fd;
                }
            }
            i++;
        }
    }
    argv[argc] = NULL;

    /* set -x: trace the expanded simple command to stderr before running it.
     * Redirections are not shown (a documented simplification).
     */
    if (sh->opt_xtrace && argc > 0) {
        fprintf(stderr, "+");
        for (int i = 0; i < argc; i++)
            fprintf(stderr, " %s", argv[i]);
        fprintf(stderr, "\n");
    }
    rc = 0;
    if (argc == 0)
        goto done;
    if (argc == 1 && is_assignment(argv[0])) {
        char *tmp = strdup(argv[0]);
        char *eq;
        if (!tmp) {
            rc = 1;
            goto done;
        }
        eq = strchr(tmp, '=');
        *eq++ = '\0';
        rc = setenv(tmp, eq, 1) == 0 ? 0 : 1;
        free(tmp);
        goto done;
    }

    /* `command NAME [args]` runs NAME as a builtin or external, bypassing a
     * shell function of the same name. `command -v/-V` is the reporting form,
     * left for run_builtin.
     */
    if (argc >= 2 && !strcmp(argv[0], "command") && strcmp(argv[1], "-v") &&
        strcmp(argv[1], "-V")) {
        for (int j = 0; j < argc; j++)
            argv[j] = argv[j + 1];
        argc--;
        skip_func = 1;
    }

    /* A pending here-document feeds stdin of the pipeline's first stage (the
     * one with no upstream pipe, in_fd < 0), unless an explicit `<` overrides
     * it.
     */
    if (own_in < 0 && in_fd < 0 && sh->heredoc_fd >= 0)
        in_fd = sh->heredoc_fd;
    in_fd = own_in >= 0 ? own_in : in_fd;
    out_fd = own_out >= 0 ? own_out : out_fd;
    err_fd = own_err >= 0 ? own_err : err_fd;
    if (!strcmp(argv[0], "exec")) {
        rc = builtin_exec(argv, in_fd, out_fd, err_fd);
        handled = 1;
    } else {
        int saved[3];

        if (sh_redir_apply(in_fd, out_fd, err_fd, saved) != 0) {
            fprintf(stderr, "sh: redirection failed: %s\n", strerror(errno));
            sh_redir_restore(saved);
            rc = 1;
            goto done;
        }
        rc = run_builtin(sh, argv, argc, in_fd, &handled);
        if (!handled && !skip_func) {
            struct sh_func *fn = sh_find_func(sh, argv[0]);

            if (fn) {
                rc = run_function(sh, fn, argv, argc);
                handled = 1;
            }
        }
        sh_redir_restore(saved);
    }
    if (!handled)
        rc = run_external(argv, in_fd, out_fd, err_fd, background, pid_out);
    goto done;

open_fail:
    fprintf(stderr, "sh: %s: %s\n", open_path ? open_path : "?",
            strerror(errno));
    rc = 1;
done:
    close_if_needed(own_in);
    close_if_needed(own_out);
    close_if_needed(own_err);
    return rc;
}

static int run_pipeline(struct sh *sh,
                        struct sh_token *tok,
                        int a,
                        int b,
                        int background)
{
    pid_t pids[SH_MAX_TOKENS];
    int npids = 0;
    int start = a;
    int input = -1;
    int rc = 0;
    int last_status = 0;
    pid_t last_pid = -1;
    bool stopped = false;

    sh_jc_pgid = 0;
    for (int i = a; i <= b; i++) {
        if (i == b || tok[i].op == SH_PIPE) {
            int pfd[2] = {
                [0] = -1,
                [1] = -1,
            };
            int out = -1;
            int is_last = i == b;
            pid_t pid = -1;

            if (!is_last) {
                if (pipe(pfd) != 0) {
                    perror("pipe");
                    close_if_needed(input);
                    rc = 1;
                    goto wait_and_return;
                }
                out = pfd[1];
            }
            if (background) {
                rc = command_from_tokens(sh, tok, start, i, input, out, -1, 1,
                                         NULL);
            } else if (!is_last) {
                /* Foreground intermediate: spawn without waiting; the shell
                 * waits after the last stage exits.
                 */
                rc = command_from_tokens(sh, tok, start, i, input, out, -1, 0,
                                         &pid);
                if (pid > 0 && npids < (int) (sizeof(pids) / sizeof(pids[0])))
                    pids[npids++] = pid;
            } else {
                /* Foreground last: spawn, then wait below for the exit status.
                 * Builtins set pid=-1 and return the status in rc.
                 */
                rc = command_from_tokens(sh, tok, start, i, input, out, -1, 0,
                                         &pid);
                if (pid > 0)
                    last_pid = pid;
                else
                    last_status = rc;
            }
            close_if_needed(input);
            close_if_needed(out);
            input = pfd[0];
            start = i + 1;
        }
    }
    close_if_needed(input);

wait_and_return:
    /* Wait on the foreground pipeline stage and reap the rest. */
    if (last_pid > 0) {
        if (sh_jc) {
            last_status = sh_wait_fg(last_pid, sh_jc_pgid, sh_jc_cmd, &stopped);
            sh_take_terminal();
        } else {
            int st = 0;
            if (waitpid(last_pid, &st, 0) == last_pid && WIFEXITED(st))
                last_status = WEXITSTATUS(st);
            else
                last_status = 128;
        }
    }
    /* A stopped job's other stages are stopped with it; do not wait for them. */
    for (int i = 0; i < npids && !stopped; i++) {
        int st;
        if (pids[i] == last_pid)
            continue;
        waitpid(pids[i], &st, 0);
    }
    sh_reap_zombies();
    return rc != 0 ? rc : last_status;
}

/* One statement split out of a line: its NUL-terminated text plus the operator
 * that follows it (SH_SEMI, SH_AND, SH_OR, or SH_BG for a trailing `&`).
 */
struct sh_stmt {
    char *text;
    enum sh_op sep;
};

static char *dup_range(const char *s, size_t n)
{
    char *r = malloc(n + 1);

    if (r) {
        memcpy(r, s, n);
        r[n] = '\0';
    }
    return r;
}

/* Split a line into statements at top-level `;`, `&&`, `||`, and a single `&`
 * (background), ignoring separators inside quotes or after a backslash. A pipe
 * (`|`), redirections, and their operands stay inside the statement. run_line
 * tokenizes -- and therefore parameter-expands -- each statement only when it
 * is about to run, so a variable or `$?` set by an earlier statement is visible
 * to a later one (POSIX per-command expansion).
 *
 * Returns the count, or -1 on overflow / allocation failure.
 */
static int split_statements(const char *line, struct sh_stmt *out, int max)
{
    const char *seg = line;
    const char *p = line;
    int n = 0;
    int quote = 0;

    for (;;) {
        char c = *p;

        if (c == '\0')
            break;
        if (quote == '\'') {
            if (c == '\'')
                quote = 0;
            p++;
            continue;
        }
        if (c == '\\') {
            if (p[1] == '\0') {
                p++;
                break;
            }
            p += 2;
            continue;
        }
        if (c == '\'' && quote != '"') {
            quote = '\'';
            p++;
            continue;
        }
        if (c == '"') {
            quote = quote == '"' ? 0 : '"';
            p++;
            continue;
        }
        if (quote != 0) {
            p++;
            continue;
        }

        /* Unquoted below. Skip over command substitutions and ${...} so a `;`,
         * `&&`, or `||` inside them is not mistaken for a statement separator.
         */
        if (c == '`') {
            p++;
            while (*p != '\0' && *p != '`') {
                if (*p == '\\' && p[1] != '\0')
                    p++;
                p++;
            }
            if (*p == '`')
                p++;
            continue;
        }
        if (c == '$' && p[1] == '(') {
            int depth = 1;
            int q2 = 0;

            p += 2;
            while (*p != '\0') {
                if (q2) {
                    if (q2 == '"' && *p == '\\' && p[1] != '\0')
                        p++; /* skip escaped char inside double quotes */
                    else if (*p == q2)
                        q2 = 0;
                } else if (*p == '\\' && p[1] != '\0') {
                    p++;
                } else if (*p == '\'' || *p == '"') {
                    q2 = *p;
                } else if (*p == '(') {
                    depth++;
                } else if (*p == ')' && --depth == 0) {
                    p++;
                    break;
                }
                p++;
            }
            continue;
        }
        if (c == '$' && p[1] == '{') {
            int depth = 1;

            p += 2;
            while (*p != '\0') {
                if (*p == '{')
                    depth++;
                else if (*p == '}' && --depth == 0) {
                    p++;
                    break;
                }
                p++;
            }
            continue;
        }

        /* A `#` starts a comment only at a word boundary, so it does not
         * mistake the `#` in a `${VAR#pat}` expansion for one; a `2>&N`
         * redirection's `&` is not a separator.
         */
        if (c == '#' && (p == line || strchr(" \t;&|<>()", p[-1]) != NULL))
            break;
        if (c == '2' && p[1] == '>' && p[2] == '&') {
            p += 3;
            while (isdigit((unsigned char) *p))
                p++;
            continue;
        }
        if (c == ';' || c == '&' || (c == '|' && p[1] == '|')) {
            enum sh_op sep = SH_SEMI;
            int adv = 1;

            if (c == '&' && p[1] == '&') {
                sep = SH_AND;
                adv = 2;
            } else if (c == '&') {
                sep = SH_BG;
            } else if (c == '|') {
                sep = SH_OR;
                adv = 2;
            }
            if (n >= max || !(out[n].text = dup_range(seg, (size_t) (p - seg))))
                goto fail;
            out[n++].sep = sep;
            p += adv;
            seg = p;
            continue;
        }
        p++;
    }
    if (n >= max || !(out[n].text = dup_range(seg, (size_t) (p - seg))))
        goto fail;
    out[n++].sep = SH_SEMI;
    return n;

fail:
    while (n > 0)
        free(out[--n].text);
    return -1;
}

static int run_line(struct sh *sh, const char *line)
{
    struct sh_stmt *stmt = malloc(sizeof(*stmt) * SH_MAX_TOKENS);
    enum sh_op prev = SH_SEMI;
    int nstmt;
    int rc = 0;

    if (!stmt) {
        fprintf(stderr, "sh: out of memory\n");
        return 2;
    }
    nstmt = split_statements(line, stmt, SH_MAX_TOKENS);
    if (nstmt < 0) {
        free(stmt);
        fprintf(stderr, "sh: parse error\n");
        return 2;
    }
    for (int i = 0; i < nstmt; i++) {
        int run = !((prev == SH_AND && rc != 0) || (prev == SH_OR && rc == 0));

        if (run) {
            struct sh_token tok[SH_MAX_TOKENS];
            int ntok;

            sh->expand_abort = false;
            ntok = tokenize(sh, stmt[i].text, tok);

            if (ntok < 0) {
                fprintf(stderr, "sh: parse error\n");
                rc = 2;
            } else if (sh->exit_requested || sh->expand_abort) {
                /* A ${x:?}/${x:=} or `set -u` expansion failure: abort this
                 * statement pre-exec and keep the status it set. exit_requested
                 * also ends a non-interactive shell; expand_abort skips just
                 * the command so an interactive `set -u` keeps the prompt
                 * alive.
                 */
                rc = sh->last_status;
                token_free(tok, ntok);
            } else {
                if (ntok > 0)
                    rc = run_pipeline(sh, tok, 0, ntok, stmt[i].sep == SH_BG);
                token_free(tok, ntok);
            }

            /* Publish the status now so a following statement's $? expansion,
             * which tokenize() reads from sh->last_status, sees this result.
             */
            sh->last_status = rc;

            /* set -e: exit on a failed command that ends an AND-OR list (sep is
             * `;`, so not a `&&`/`||` non-final operand, nor a background `&`),
             * unless we are in a suppressed context (an if/while/until
             * condition). This is where errexit lives -- a body's failed simple
             * command aborts the enclosing run_script_range via exit_requested.
             */
            if (sh->opt_errexit && !sh->errexit_suppress && rc != 0 &&
                stmt[i].sep == SH_SEMI)
                sh->exit_requested = true;
        }
        prev = stmt[i].sep;
        if (sh->exit_requested)
            break;
    }
    for (int i = 0; i < nstmt; i++)
        free(stmt[i].text);
    free(stmt);
    sh->last_status = rc;
    return rc;
}

static int script_add(struct sh_script *sc, const char *line)
{
    char **nl;

    if (sc->count == sc->cap) {
        int cap = sc->cap == 0 ? 32 : sc->cap * 2;
        nl = realloc(sc->line, (size_t) cap * sizeof(*nl));
        if (!nl)
            return -1;
        sc->line = nl;
        sc->cap = cap;
    }
    sc->line[sc->count] = strdup(line);
    if (!sc->line[sc->count])
        return -1;
    sc->count++;
    return 0;
}

static void script_free(struct sh_script *sc)
{
    for (int i = 0; i < sc->count; i++)
        free(sc->line[i]);
    free(sc->line);
    sc->line = NULL;
    sc->count = 0;
    sc->cap = 0;
}

static struct sh_func *sh_find_func(struct sh *sh, const char *name)
{
    for (int i = 0; i < sh->nfuncs; i++)
        if (!strcmp(sh->funcs[i].name, name))
            return &sh->funcs[i];
    return NULL;
}

/* Install (or replace) a function `name` with body lines [from,to) of sc. */
static int sh_add_func(struct sh *sh,
                       const char *name,
                       struct sh_script *sc,
                       int from,
                       int to)
{
    struct sh_script body = {0};
    struct sh_func *fn;

    /* Build the whole new body first; touch existing state only once it is
     * complete, so an allocation failure can never corrupt a live function.
     */
    for (int i = from; i < to; i++) {
        if (script_add(&body, sc->line[i]) != 0) {
            script_free(&body);
            return -1;
        }
    }
    fn = sh_find_func(sh, name);
    if (fn) {
        script_free(&fn->body);
        fn->body = body;
        return 0;
    }
    if (sh->nfuncs == sh->funcs_cap) {
        int cap = sh->funcs_cap == 0 ? 8 : sh->funcs_cap * 2;
        struct sh_func *nf = realloc(sh->funcs, (size_t) cap * sizeof(*nf));

        if (!nf) {
            script_free(&body);
            return -1;
        }
        sh->funcs = nf;
        sh->funcs_cap = cap;
    }
    fn = &sh->funcs[sh->nfuncs];
    fn->name = strdup(name);
    if (!fn->name) {
        script_free(&body);
        return -1;
    }
    fn->body = body;
    sh->nfuncs++;
    return 0;
}

/* Free all defined functions (final shutdown). */
static void sh_free_funcs(struct sh *sh)
{
    for (int i = 0; i < sh->nfuncs; i++) {
        free(sh->funcs[i].name);
        script_free(&sh->funcs[i].body);
    }
    free(sh->funcs);
    sh->funcs = NULL;
    sh->nfuncs = 0;
    sh->funcs_cap = 0;
}

/* Count net brace depth contributed by a line. Naive: every { and } counts,
 * including those inside quotes or comments, so a body line like `echo "}"`
 * would end the function early. Full accuracy needs tokenization; this suffices
 * for typical function bodies that do not put literal braces in strings.
 */
static int brace_delta(const char *s)
{
    int d = 0;

    for (const char *c = s; *c != '\0'; c++) {
        if (*c == '{')
            d++;
        else if (*c == '}')
            d--;
    }
    return d;
}

/* If sc->line[*ip] begins a function definition `name() ...`, capture its body
 * into the function table and advance *ip past the closing `}`.
 *
 * Returns 1 if a definition was consumed, 0 if the line is not one, -1 on
 * error. Handles the body brace on the def line, on the next line, and a
 * one-line `name() { ...; }`.
 */
static int heredoc_skip(struct sh_script *sc, int i, int to, char *line);

static int capture_func_def(struct sh *sh,
                            struct sh_script *sc,
                            int *ip,
                            int to)
{
    char line[SH_LINE_MAX];
    char name[64];
    const char *p;
    size_t nl = 0;
    int body_from;
    int end_i;

    snprintf(line, sizeof(line), "%s", sc->line[*ip]);
    trim(line);
    p = line;
    if (!(isalpha((unsigned char) *p) || *p == '_'))
        return 0;
    while (is_name_char((unsigned char) *p) && nl < sizeof(name) - 1)
        name[nl++] = *p++;
    name[nl] = '\0';
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '(')
        return 0;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ')')
        return 0;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '{') {
        /* Brace on the def line. One-liner if a matching } is on this line. */
        p++;
        if (brace_delta(p) < 0) {
            struct sh_script one = {0};
            char body[SH_LINE_MAX];
            char *rb;

            snprintf(body, sizeof(body), "%s", p);
            rb = strrchr(body, '}');
            if (rb)
                *rb = '\0';
            trim(body);
            if (script_add(&one, body) != 0 ||
                sh_add_func(sh, name, &one, 0, one.count) != 0) {
                script_free(&one);
                return -1;
            }
            script_free(&one);
            return 1;
        }
        body_from = *ip + 1;
        end_i = *ip; /* def line already opened one brace */
        {
            int depth = 1;
            int i;

            for (i = *ip + 1; i < to; i++) {
                char t[SH_LINE_MAX];

                depth += brace_delta(sc->line[i]);
                if (depth <= 0) {
                    end_i = i;
                    break;
                }
                snprintf(t, sizeof(t), "%s", sc->line[i]);
                trim(t);
                i = heredoc_skip(sc, i, to, t);
            }
            if (depth > 0)
                return -1;
        }
    } else if (*p == '\0') {
        /* Brace must open the next line. */
        char nlbuf[SH_LINE_MAX];
        int depth;
        int i;

        if (*ip + 1 >= to)
            return 0;
        snprintf(nlbuf, sizeof(nlbuf), "%s", sc->line[*ip + 1]);
        trim(nlbuf);
        if (nlbuf[0] != '{')
            return 0;
        body_from = *ip + 2;
        depth = brace_delta(nlbuf);
        end_i = *ip + 1;
        for (i = *ip + 2; depth > 0 && i < to; i++) {
            char t[SH_LINE_MAX];

            depth += brace_delta(sc->line[i]);
            if (depth <= 0) {
                end_i = i;
                break;
            }
            snprintf(t, sizeof(t), "%s", sc->line[i]);
            trim(t);
            i = heredoc_skip(sc, i, to, t);
        }
        if (depth > 0)
            return -1;
    } else {
        return 0;
    }
    if (sh_add_func(sh, name, sc, body_from, end_i) != 0)
        return -1;
    *ip = end_i;
    return 1;
}

static int load_script(int fd, struct sh_script *sc)
{
    char buf[256];
    char line[SH_LINE_MAX];
    size_t used = 0;
    ssize_t n;

    /* Init scripts are tiny; read(2) avoids stdio refill failures. Store each
     * line WITHOUT its terminator: a trailing `\n`, and a `\r` immediately
     * before it (CRLF), are dropped. Most consumers trim() anyway, but
     * here-document bodies use these lines verbatim, so the newline must not be
     * part of them. A lone `\r` inside a line is preserved.
     */
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];

            if (c == '\n') {
                if (used > 0 && line[used - 1] == '\r')
                    used--; /* drop a CRLF carriage return */
                line[used] = '\0';
                if (script_add(sc, line) != 0)
                    return -1;
                used = 0;
                continue;
            }
            if (used + 1 < sizeof(line))
                line[used++] = c;
            if (used + 1 == sizeof(line)) { /* over-long line: force a split */
                line[used] = '\0';
                fprintf(stderr, "sh: script line exceeds %d bytes, split\n",
                        (int) sizeof(line) - 1);
                if (script_add(sc, line) != 0)
                    return -1;
                used = 0;
            }
        }
    }
    if (n < 0)
        return -1;
    if (used != 0) {
        line[used] = '\0';
        if (script_add(sc, line) != 0)
            return -1;
    }
    return 0;
}

static bool starts_word(const char *line, const char *word)
{
    size_t n;

    while (*line == ' ' || *line == '\t')
        line++;
    n = strlen(word);
    return strncmp(line, word, n) == 0 &&
           (line[n] == '\0' || line[n] == ' ' || line[n] == '\t' ||
            line[n] == ';' || line[n] == '\n');
}

/* Scan `line` for an unquoted here-document operator (`<<` / `<<-`), skipping
 * quotes, command substitutions, and arithmetic so a `<<` inside them (e.g. the
 * shift in `$((1 << 4))`) is not mistaken for one.
 *
 * Returns a pointer to the `<<`, or NULL. A `<<<` here-string is not a
 * here-document, so it is skipped.
 */
static char *heredoc_find(char *line)
{
    char *p = line;
    int q = 0;

    for (; *p; p++) {
        char c = *p;

        if (q == '\'') {
            if (c == '\'')
                q = 0;
            continue;
        }
        if (q == '"') {
            if (c == '\\' && p[1])
                p++;
            else if (c == '"')
                q = 0;
            continue;
        }
        if (c == '\\' && p[1]) {
            p++;
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            continue;
        }
        if (c == '`') {
            for (p++; *p && *p != '`'; p++)
                if (*p == '\\' && p[1])
                    p++;
            if (!*p)
                break;
            continue;
        }
        if (c == '$' && p[1] == '(') {
            int depth = 1;
            int qq = 0;

            for (p += 2; *p; p++) {
                if (qq) {
                    if (qq == '"' && *p == '\\' && p[1])
                        p++;
                    else if (*p == qq)
                        qq = 0;
                } else if (*p == '\\' && p[1]) {
                    p++;
                } else if (*p == '\'' || *p == '"') {
                    qq = *p;
                } else if (*p == '(') {
                    depth++;
                } else if (*p == ')' && --depth == 0) {
                    break;
                }
            }
            if (!*p)
                break;
            continue;
        }
        if (c == '<' && p[1] == '<' && p[2] != '<')
            return p;
    }
    return NULL;
}

/* Parse a here-document delimiter starting at `hh` (the `<<`). Copies the word
 * into `delim`, sets *strip for `<<-`, sets *quoted if any of it was quoted (so
 * the body is not expanded), and returns the command-line remainder after the
 * word. The word ends at whitespace or a redirection/list metacharacter, so
 * `<<EOF >out` keeps `>out` in the remainder.
 */
static char *heredoc_delim(char *hh,
                           char *delim,
                           size_t dsize,
                           bool *strip,
                           bool *quoted)
{
    size_t dn = 0;
    char *q = hh + 2;

    *strip = false;
    *quoted = false;
    if (*q == '-') {
        *strip = true;
        q++;
    }
    while (*q == ' ' || *q == '\t')
        q++;
    while (*q && *q != ' ' && *q != '\t' && !strchr("<>|&;()", *q)) {
        if (*q == '\'' || *q == '"') {
            char qc = *q++;

            *quoted = true;
            while (*q && *q != qc) {
                if (dn + 1 < dsize)
                    delim[dn++] = *q;
                q++;
            }
            if (*q)
                q++;
        } else if (*q == '\\' && q[1]) {
            *quoted = true;
            q++;
            if (dn + 1 < dsize)
                delim[dn++] = *q;
            q++;
        } else {
            if (dn + 1 < dsize)
                delim[dn++] = *q;
            q++;
        }
    }
    delim[dn] = '\0';
    return q;
}

/* Reduce a raw script line to its here-document body form: for `<<-`, leading
 * tabs are stripped (they are dropped from both the body and the delimiter
 * match).
 *
 * Returns a pointer into `bl`. heredoc_skip and run_heredoc share this so they
 * agree on where a body ends.
 */
static const char *heredoc_body_line(const char *bl, bool strip)
{
    if (strip)
        while (*bl == '\t')
            bl++;
    return bl;
}

/* If trimmed script line `line` (at index i) opens a here-document, return the
 * index of its terminator line so a block scanner skips the inert body; else i.
 * A body line that leads with a reserved word must not be counted as one, so
 * find_matching/find_done/capture_func_def use this to agree with run_heredoc
 * on where a body ends.
 */
static int heredoc_skip(struct sh_script *sc, int i, int to, char *line)
{
    char delim[SH_LINE_MAX];
    bool strip;
    bool quoted;
    char *hh = heredoc_find(line);

    if (!hh)
        return i;
    heredoc_delim(hh, delim, sizeof(delim), &strip, &quoted);
    for (int j = i + 1; j < to; j++)
        if (!strcmp(heredoc_body_line(sc->line[j], strip), delim))
            return j;
    return to - 1;
}

static int find_matching(struct sh_script *sc,
                         int from,
                         int to,
                         const char *open1,
                         const char *mid,
                         const char *close,
                         int *mid_out)
{
    int depth = 0;

    *mid_out = -1;
    for (int i = from; i < to; i++) {
        char tmp[SH_LINE_MAX];
        snprintf(tmp, sizeof(tmp), "%s", sc->line[i]);
        trim(tmp);
        if (starts_word(tmp, open1))
            depth++;
        if (depth == 0 && mid && starts_word(tmp, mid))
            *mid_out = i;
        if (starts_word(tmp, close)) {
            if (depth == 0)
                return i;
            depth--;
        }
        i = heredoc_skip(sc, i, to, tmp);
    }
    return -1;
}

/* Find the `done` that closes a loop starting at `from`, treating for/while/
 * until as nested openers (they all close with `done`).
 *
 * Returns -1 if none.
 */
static int find_done(struct sh_script *sc, int from, int to)
{
    int depth = 0;

    for (int i = from; i < to; i++) {
        char tmp[SH_LINE_MAX];

        snprintf(tmp, sizeof(tmp), "%s", sc->line[i]);
        trim(tmp);
        if (starts_word(tmp, "for") || starts_word(tmp, "while") ||
            starts_word(tmp, "until"))
            depth++;
        if (starts_word(tmp, "done")) {
            if (depth == 0)
                return i;
            depth--;
        }
        i = heredoc_skip(sc, i, to, tmp);
    }
    return -1;
}

static int run_script_range(struct sh *sh,
                            struct sh_script *sc,
                            int from,
                            int to);

/* After a loop body runs, consume a pending break/continue.
 *
 * Returns 1 if the enclosing loop should stop iterating, 0 if it should
 * continue.
 */
static int loop_consume(struct sh *sh)
{
    if (sh->exit_requested || sh->return_requested)
        return 1;
    if (sh->loop_break) {
        sh->loop_break--;
        return 1;
    }
    if (sh->loop_continue) {
        sh->loop_continue--;
        return sh->loop_continue > 0; /* still >0: this level also stops */
    }
    return 0;
}

static int run_if(struct sh *sh, struct sh_script *sc, int *ip, int to)
{
    char cond[SH_LINE_MAX];
    char *thenp;
    int else_i;
    int fi_i;
    int then_i;
    int rc;

    snprintf(cond, sizeof(cond), "%s", sc->line[*ip]);
    trim(cond);
    memmove(cond, cond + 2, strlen(cond + 2) + 1);
    trim(cond);
    thenp = strstr(cond, "; then");
    if (thenp) {
        *thenp = '\0';
        then_i = *ip + 1;
    } else {
        size_t clen = strlen(cond);

        if (clen >= 4 && !strcmp(cond + clen - 4, "then")) {
            cond[clen - 4] = '\0';
            then_i = *ip + 1;
        } else {
            then_i = *ip + 2;
        }
    }
    trim(cond);
    sh->errexit_suppress++; /* set -e is ignored while testing the condition */
    rc = run_line(sh, cond);
    sh->errexit_suppress--;
    fi_i = find_matching(sc, *ip + 1, to, "if", "else", "fi", &else_i);
    if (fi_i < 0)
        return 2;
    if (rc == 0)
        rc = run_script_range(sh, sc, then_i, else_i >= 0 ? else_i : fi_i);
    else if (else_i >= 0)
        rc = run_script_range(sh, sc, else_i + 1, fi_i);
    *ip = fi_i;
    return rc;
}

static int run_for(struct sh *sh, struct sh_script *sc, int *ip, int to)
{
    struct sh_token tok[SH_MAX_TOKENS];
    struct sh_words vals = {0};
    char *var = NULL;
    int ntok;
    int done_i;
    int rc = 0;

    ntok = tokenize(sh, sc->line[*ip], tok);

    /* tok[1] (loop var) and tok[2] ("in") must be words: an operator token
     * carries s == NULL, so strcmp/strdup on it would dereference NULL. Reject
     * `for` forms this shell does not accept (e.g. `for i; do`) as a syntax
     * error rather than crashing.
     */
    if (ntok < 4 || tok[1].op != SH_WORD || tok[2].op != SH_WORD ||
        strcmp(tok[0].s, "for") != 0 || strcmp(tok[2].s, "in") != 0) {
        token_free(tok, ntok > 0 ? ntok : 0);
        return 2;
    }
    var = strdup(tok[1].s);
    for (int i = 3; i < ntok; i++) {
        if (tok[i].op == SH_WORD && strcmp(tok[i].s, "do") != 0) {
            if (words_add_fields(&vals, tok[i].s) != 0) {
                token_free(tok, ntok);
                free(var);
                words_free(&vals);
                return 1;
            }
        }
    }
    token_free(tok, ntok);
    done_i = find_done(sc, *ip + 1, to);
    if (done_i < 0) {
        free(var);
        words_free(&vals);
        return 2;
    }
    sh->loop_depth++;
    for (int i = 0; i < vals.n && !sh->exit_requested; i++) {
        setenv(var, vals.v[i], 1);
        rc = run_script_range(sh, sc, *ip + 1, done_i);
        if (loop_consume(sh))
            break;
    }
    sh->loop_depth--;
    free(var);
    words_free(&vals);
    *ip = done_i;
    return rc;
}

/* Find the loop `do` keyword in a condition string: a bare word "do", not
 * inside quotes, preceded by start / whitespace / `;` and followed by end /
 * whitespace / `;`.
 *
 * Returns a pointer to it, or NULL. Limitation: it matches the first such word,
 * so a `do` used as a plain command argument (e.g. `while echo do; do`)
 * mis-cuts; telling reserved-word position from argument position would need
 * full tokenization, which this line-based parser does not do.
 */
static char *find_do_kw(char *s)
{
    int q = 0;

    for (char *p = s; *p != '\0'; p++) {
        if (q) {
            if (*p == q)
                q = 0;
            continue;
        }
        if (*p == '\'' || *p == '"') {
            q = *p;
            continue;
        }
        if (*p == '\\' && p[1] != '\0') {
            p++;
            continue;
        }
        if (p[0] == 'd' && p[1] == 'o' &&
            (p == s || p[-1] == ' ' || p[-1] == '\t' || p[-1] == ';') &&
            (p[2] == '\0' || p[2] == ' ' || p[2] == '\t' || p[2] == ';'))
            return p;
    }
    return NULL;
}

/* while/until COND; do BODY; done. `until` inverts the condition sense. */
static int run_while(struct sh *sh,
                     struct sh_script *sc,
                     int *ip,
                     int to,
                     int until)
{
    char cond[SH_LINE_MAX];
    char *dop;
    int do_i;
    int done_i;
    int rc = 0;

    snprintf(cond, sizeof(cond), "%s", sc->line[*ip]);
    trim(cond);
    memmove(cond, cond + 5, strlen(cond + 5) + 1); /* drop "while"/"until" */
    trim(cond);
    dop = find_do_kw(cond);
    if (dop) {
        /* same-line `do`: cut it and any trailing separators off the condition
         */
        while (dop > cond &&
               (dop[-1] == ' ' || dop[-1] == '\t' || dop[-1] == ';'))
            dop--;
        *dop = '\0';
        do_i = *ip + 1;
    } else {
        /* `do` must be alone on the next line, else the loop is malformed */
        char nl[SH_LINE_MAX];

        if (*ip + 1 >= to)
            return 2;
        snprintf(nl, sizeof(nl), "%s", sc->line[*ip + 1]);
        trim(nl);
        if (strcmp(nl, "do") != 0)
            return 2;
        do_i = *ip + 2;
    }
    done_i = find_done(sc, *ip + 1, to);
    if (done_i < 0)
        return 2;
    sh->loop_depth++;
    while (!sh->exit_requested) {
        int ok;

        sh->errexit_suppress++; /* set -e ignored while testing the condition */
        ok = run_line(sh, cond) == 0;
        sh->errexit_suppress--;
        if (until ? ok : !ok)
            break;
        rc = run_script_range(sh, sc, do_i, done_i);
        if (loop_consume(sh))
            break;
    }
    sh->loop_depth--;
    *ip = done_i;
    return rc;
}

static int run_case(struct sh *sh, struct sh_script *sc, int *ip, int to)
{
    struct sh_token tok[SH_MAX_TOKENS];
    char *word = NULL;
    int ntok;
    int esac_i;
    int mid;
    int rc = 0;

    ntok = tokenize(sh, sc->line[*ip], tok);

    /* tok[1] (the case word) must be a word; an operator token has s == NULL,
     * so strdup(NULL) would crash. Reject `case ; esac` as a syntax error.
     */
    if (ntok < 3 || tok[1].op != SH_WORD || strcmp(tok[0].s, "case") != 0) {
        token_free(tok, ntok > 0 ? ntok : 0);
        return 2;
    }
    word = strdup(tok[1].s);
    token_free(tok, ntok);
    if (!word)
        return 1;
    esac_i = find_matching(sc, *ip + 1, to, "case", NULL, "esac", &mid);
    if (esac_i < 0) {
        free(word);
        return 2;
    }
    for (int i = *ip + 1; i < esac_i && !sh->exit_requested; i++) {
        char line[SH_LINE_MAX];
        char *rp;
        snprintf(line, sizeof(line), "%s", sc->line[i]);
        trim(line);
        rp = strchr(line, ')');
        if (!rp)
            continue;
        *rp++ = '\0';
        trim(line);
        if (fnmatch(line, word, 0) == 0) {
            char *end = strstr(rp, ";;");
            if (end)
                *end = '\0';
            trim(rp);
            rc = run_line(sh, rp);
            break;
        }
    }
    free(word);
    *ip = esac_i;
    return rc;
}

/* Expand a here-document body: parameter, command, and arithmetic expansions
 * only -- no quote removal, word splitting, or globbing, per POSIX. In an
 * unquoted body a backslash escapes only $, `, and \.
 *
 * Returns a malloc'd string, or NULL on allocation failure.
 */
static char *expand_string(struct sh *sh, const char *s)
{
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    const char *p = s;

    while (*p) {
        if (*p == '\\' && (p[1] == '$' || p[1] == '`' || p[1] == '\\')) {
            p++;
            if (append_char(&buf, &len, &cap, *p++) != 0)
                goto oom;
            continue;
        }
        if (p[0] == '$' && p[1] == '(' && p[2] == '(') {
            char *expr;
            char nb[32];
            struct arith_ctx a;
            long val;
            int closed;

            p += 3;
            expr = read_arith(&p, &closed);
            if (!expr)
                goto oom;
            a.p = expr;
            a.sh = sh;
            a.err = 0;
            a.dead = 0;
            a.depth = 0;
            val = arith_lor(&a);
            arith_skip(&a);
            if (!closed || a.err || *a.p != '\0')
                val = 0;
            free(expr);
            snprintf(nb, sizeof(nb), "%ld", val);
            if (append_str(&buf, &len, &cap, nb) != 0)
                goto oom;
            continue;
        }
        if ((p[0] == '$' && p[1] == '(') || p[0] == '`') {
            char *inner;
            char *result;

            if (*p == '`') {
                p++;
                inner = read_backtick(&p);
            } else {
                p += 2;
                inner = read_paren_sub(&p);
            }
            if (!inner)
                goto oom;
            if (capture_command(inner, &result) != 0) {
                free(inner);
                free(result);
                goto oom;
            }
            free(inner);
            if (append_str(&buf, &len, &cap, result) != 0) {
                free(result);
                goto oom;
            }
            free(result);
            continue;
        }
        if (*p == '$') {
            p++;
            if (expand_dollar(sh, &p, &buf, &len, &cap) != 0)
                goto oom;
            continue;
        }
        if (append_char(&buf, &len, &cap, *p++) != 0)
            goto oom;
    }
    if (!buf)
        buf = strdup("");
    return buf;

oom:
    free(buf);
    return NULL;
}

/* Run a here-document command at line index *ip. `line` is the trimmed line and
 * `hh` points at its `<<`. Gathers the body from the following script lines up
 * to the delimiter (stripping leading tabs for `<<-`), expands it unless the
 * delimiter is quoted, feeds it to the command over a pipe, and advances *ip to
 * the delimiter line.
 *
 * Returns the command's status.
 *
 * Bounds vs full POSIX (rare constructs, kept simple deliberately): the body is
 * attached to the pipeline's first stage, so `a | b <<EOF` feeds `a` rather
 * than `b`; an explicit `<` always wins regardless of left-to-right order; only
 * one `<<` per command line is consumed (a second is rejected); and on a
 * multi-statement line like `cat <<EOF; grep x` the body binds to the first
 * consumer (the rest see EOF) rather than only to `cat`.
 */
static int run_heredoc(struct sh *sh,
                       struct sh_script *sc,
                       int *ip,
                       int to,
                       char *line,
                       char *hh)
{
    char delim[SH_LINE_MAX];
    bool strip;
    bool quoted;
    char *q;
    char *cmd;
    char *body = NULL;
    size_t blen = 0;
    size_t bcap = 0;
    size_t n;
    int i;
    int pfd[2];
    int rc;

    q = heredoc_delim(hh, delim, sizeof(delim), &strip, &quoted);

    /* Rebuild the command line without the `<< delim` operand. */
    *hh = '\0';
    cmd = malloc(strlen(line) + strlen(q) + 1);
    if (!cmd)
        return 2;
    strcpy(cmd, line);
    strcat(cmd, q);

    /* Only one here-document per command line is supported; a second `<<` would
     * reach run_line() as a stray redirection, so reject it with a diagnostic.
     */
    if (heredoc_find(cmd)) {
        fprintf(stderr,
                "sh: multiple here-documents on one command "
                "are not supported\n");
        free(cmd);
        *ip = *ip + 1 < to ? *ip + 1 : to - 1;
        return 1;
    }

    /* Gather body lines up to one equal to the delimiter, bounded by `to`. */
    for (i = *ip + 1; i < to; i++) {
        const char *bl = heredoc_body_line(sc->line[i], strip);

        if (!strcmp(bl, delim))
            break;
        if (append_str(&body, &blen, &bcap, bl) != 0 ||
            append_char(&body, &blen, &bcap, '\n') != 0) {
            free(body);
            free(cmd);
            return 2;
        }
    }
    if (i >= to)
        fprintf(stderr,
                "sh: warning: here-document delimited by end-of-file "
                "(wanted `%s')\n",
                delim);
    if (!body)
        body = strdup("");
    if (!body) {
        free(cmd);
        return 2;
    }
    if (!quoted) {
        char *ex = expand_string(sh, body);

        free(body);
        if (!ex) {
            free(cmd);
            return 2;
        }
        body = ex;
    }

    /* Feed the body over a pipe. The reader (a builtin like `read`, or a
     * spawned external) may not drain until run_line runs below, so a body
     * larger than the pipe buffer would deadlock the single-threaded write; cap
     * it at PIPE_BUF. A pipe() failure is a redirection failure -- do not run
     * the command, or it would silently read the shell's inherited stdin.
     */
    if (pipe(pfd) != 0) {
        fprintf(stderr, "sh: here-document: %s\n", strerror(errno));
        free(body);
        free(cmd);
        *ip = i < to ? i : to - 1;
        return 1;
    }

    /* Keep the read-end out of any child spawned while heredoc_fd is live (a
     * later pipeline stage, or a `$(...)` inside cmd): the intended consumer
     * still receives it via an explicit dup2 onto stdin, which clears CLOEXEC.
     */
    (void) fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
    n = strlen(body);
    if (n > PIPE_BUF) {
        fprintf(stderr, "sh: here-document truncated to %d bytes\n", PIPE_BUF);
        n = PIPE_BUF;
    }
    rc = 0;
    for (size_t off = 0; off < n;) {
        ssize_t w = write(pfd[1], body + off, n - off);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "sh: here-document: %s\n", strerror(errno));
            rc = 1;
            break;
        }
        off += (size_t) w;
    }
    close(pfd[1]);
    if (rc == 0) {
        sh->heredoc_fd = pfd[0];
        rc = run_line(sh, cmd);
        sh->heredoc_fd = -1;
    }
    close(pfd[0]);
    free(body);
    free(cmd);
    *ip = i < to ? i : to - 1;
    return rc;
}

static int run_script_range(struct sh *sh,
                            struct sh_script *sc,
                            int from,
                            int to)
{
    int rc = 0;

    for (int i = from; i < to && !sh->exit_requested && !sh->return_requested &&
                       !sh->loop_break && !sh->loop_continue;
         i++) {
        char line[SH_LINE_MAX];
        int def;
        snprintf(line, sizeof(line), "%s", sc->line[i]);
        trim(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (starts_word(line, "then") || starts_word(line, "do") ||
            starts_word(line, "else") || starts_word(line, "fi") ||
            starts_word(line, "done") || starts_word(line, "esac"))
            continue;
        def = capture_func_def(sh, sc, &i, to);
        if (def != 0) {
            if (def < 0)
                rc = 2;
            continue;
        }
        if (starts_word(line, "if"))
            rc = run_if(sh, sc, &i, to);
        else if (starts_word(line, "for"))
            rc = run_for(sh, sc, &i, to);
        else if (starts_word(line, "while"))
            rc = run_while(sh, sc, &i, to, 0);
        else if (starts_word(line, "until"))
            rc = run_while(sh, sc, &i, to, 1);
        else if (starts_word(line, "case"))
            rc = run_case(sh, sc, &i, to);
        else {
            char *hh = heredoc_find(line);

            if (hh)
                rc = run_heredoc(sh, sc, &i, to, line, hh);
            else
                rc = run_line(sh, line);
        }
    }
    return rc;
}

/* Invoke a function: set $1.. to the call arguments (saving/restoring the
 * caller's positional parameters), run the body, and consume a pending
 * `return`. The function runs in the shell process.
 */
static int run_function(struct sh *sh,
                        struct sh_func *fn,
                        char **argv,
                        int argc)
{
    char **saved_params = sh->params;
    int saved_n = sh->nparams;
    int rc;

    sh->params = NULL; /* detach so sh_set_params does not free the saved set */
    sh->nparams = 0;
    if (sh_set_params(sh, argv + 1, argc - 1) != 0) {
        sh->params = saved_params;
        sh->nparams = saved_n;
        return 1;
    }
    sh->fn_depth++;
    rc = run_script_range(sh, &fn->body, 0, fn->body.count);
    sh->fn_depth--;
    for (int i = 0; i < sh->nparams; i++)
        free(sh->params[i]);
    free(sh->params);
    sh->params = saved_params;
    sh->nparams = saved_n;
    if (sh->return_requested) {
        sh->return_requested = false;
        rc = sh->return_status;
    }
    return rc;
}

static int run_file(struct sh *sh, const char *path)
{
    struct sh_script sc = {0};
    int rc;
    int fd;

    /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "sh: %s: %s\n", path, strerror(errno));
        return 1;
    }
    rc = load_script(fd, &sc);
    close(fd);
    if (rc != 0) {
        script_free(&sc);
        return 1;
    }
    rc = run_script_range(sh, &sc, 0, sc.count);
    script_free(&sc);
    return rc;
}

static int run_interactive(struct sh *sh)
{
    char line[SH_LINE_MAX];
    int rc = 0;
    const char *prompt;
    struct sigaction sa_ign;
    struct sigaction sa_int_old;
    struct sigaction sa_quit_old;

    sh->interactive = true;
    /* Ignore the terminal's signals: ^C, ^\ and ^Z are for the jobs. */
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sigaction(SIGINT, &sa_ign, &sa_int_old);
    sigaction(SIGQUIT, &sa_ign, &sa_quit_old);
    sigaction(SIGTSTP, &sa_ign, NULL);
    sigaction(SIGTTIN, &sa_ign, NULL);
    sigaction(SIGTTOU, &sa_ign, NULL);

    /* Job control needs the terminal to be ours: take a group of our own and
     * make it the foreground. Without a controlling terminal (tcsetpgrp fails)
     * the shell runs jobs in its own group as before.
     */
    sh_shell_pgid = getpgrp();
    pid_t old_pgid = sh_shell_pgid;

    if (isatty(STDIN_FILENO) && tcgetpgrp(STDIN_FILENO) == old_pgid) {
        if (getpgrp() == getpid() || setpgid(0, 0) == 0) {
            sh_shell_pgid = getpgrp();
            if (tcsetpgrp(STDIN_FILENO, sh_shell_pgid) == 0)
                sh_jc = true;
            else
                (void) setpgid(0, old_pgid);
        }
    }

    while (!sh->exit_requested) {
        sh_reap_zombies();
        prompt = geteuid() == 0 ? "# " : "$ ";
        if (sh_read_line(sh, prompt, line, sizeof(line)) != 0)
            break;
        trim(line);
        if (line[0] == '\0')
            continue;
        sh_add_history(sh, line);
        rc = run_line(sh, line);
    }
    if (sh_jc) {
        /* Hand the terminal back to the group we took it from (login, getty). */
        (void) tcsetpgrp(STDIN_FILENO, old_pgid);
        sh_jc = false;
    }
    sigaction(SIGINT, &sa_int_old, NULL);
    sigaction(SIGQUIT, &sa_quit_old, NULL);
    return rc;
}

int toolbox_sh_main(int argc, char **argv)
{
    struct sh sh;
    int opt;
    const char *command = NULL;

    memset(&sh, 0, sizeof(sh));
    sh.heredoc_fd = -1;
    optind = 1;
    while ((opt = getopt(argc, argv, "ic:")) != -1) {
        switch (opt) {
        case 'i':
            sh.interactive = true;
            break;
        case 'c':
            command = optarg;
            break;
        default:
            fprintf(stderr, "usage: sh [-i] [-c COMMAND] [SCRIPT]\n");
            return 2;
        }
    }
    if (!getenv("PATH"))
        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    if (command) {
        /* sh -c COMMAND [name [arg...]] */
        if (optind < argc) {
            sh.arg0 = argv[optind];
            sh_set_params(&sh, argv + optind + 1, argc - optind - 1);
        } else {
            sh.arg0 = argv[0];
        }
        opt = run_line(&sh, command);
    } else if (optind < argc) {
        /* sh SCRIPT [arg...] */
        sh.arg0 = argv[optind];
        sh_set_params(&sh, argv + optind + 1, argc - optind - 1);
        opt = run_file(&sh, argv[optind]);
    } else {
        sh.arg0 = argv[0];
        opt = run_interactive(&sh);
    }
    sh_free_history(&sh);
    sh_free_funcs(&sh);
    for (int i = 0; i < sh.nparams; i++)
        free(sh.params[i]);
    free(sh.params);
    return opt;
}
