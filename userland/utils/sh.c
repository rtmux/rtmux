/*
 * toolbox sh - compact command shell applet.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "toolbox.h"
#include "sh-scan.h"

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
#include <sys/select.h>
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
    SH_DUPOUT, /* >&N */
    SH_DUPERR, /* 2>&N */
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

/* The conditions `trap` takes; EXIT is the shell's exit, not a signal. */
static const struct {
    const char *name;
    int sig;
} sh_trap_sigs[] = {
    {"EXIT", 0},
    {"HUP", SIGHUP},
    {"INT", SIGINT},
    {"TERM", SIGTERM},
};

#define SH_NTRAPS ((int) (sizeof(sh_trap_sigs) / sizeof(sh_trap_sigs[0])))

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
    int subst_status; /* status of the last command substitution, -1: none */
    char *traps[SH_NTRAPS]; /* trap actions, see sh_trap_sigs; NULL: none */
    bool trap_inherited[SH_NTRAPS]; /* from a parent: listed, never run */
};

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
static pid_t sh_last_bg;    /* $!: the most recent background process */
static pid_t sh_top_pid;    /* $$ when inherited from a parent shell */

/* $$: the shell's pid, which a subshell inherits from its parent. */
static pid_t sh_pid(void)
{
    return sh_top_pid ? sh_top_pid : getpid();
}

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
    SH_KEY_INTR, /* the read was interrupted by a signal */
};

static void sh_run_traps(struct sh *sh);
static bool sh_traps_pending(void);
#ifdef SH_TEST_AWAIT_WINDOW
void SH_TEST_AWAIT_WINDOW(void);
#endif
static int run_file(struct sh *sh, const char *path);
static int run_line(struct sh *sh, const char *line);
static int run_text(struct sh *sh, const char *text);
static bool starts_word(const char *line, const char *word);
static int case_explode(const char *line, struct sh_script *out);
static int script_explode(struct sh_script *sc);
static int sh_assign(const char *word, char **saved);
static char *dup_range(const char *s, size_t n);
static struct sh_func *sh_find_func(struct sh *sh, const char *name);
static int run_function(struct sh *sh,
                        struct sh_func *fn,
                        char **argv,
                        int argc);

/* How a ${...} expansion's result will be used (expand_dollar, expand_braced).
 */
#define XF_SPLIT                                                          \
    1           /* it is field-split: a quoted blank is marked, see below \
                 */
#define XF_DQ 2 /* it is inside double quotes */

/* A quoted IFS character in a ${...} result that is going to be field-split is
 * preceded by this byte, which emit_sub_result removes, so it does not split
 * the field; so is a data byte equal to it.
 */
#define SH_QUOTED_BLANK '\001'

/* After that byte, this one stands for an empty quoted string in such a result:
 * a field of its own, though it has no character.
 */
#define SH_QUOTED_EMPTY '\002'

/* In a field being built, this byte stands before a quoted character that
 * pathname expansion would take for part of a pattern (* ? [ or \), so that a
 * word with a quoted and an unquoted one is a pattern in which only the
 * unquoted one is. emit_field removes it. (One before any other byte is data.)
 */
#define SH_QUOTED_GLOB '\003'
#define SH_GLOB_CHARS "*?[\\"

/* In the fds of a command, a descriptor that `>&-` closes (-1 is "not set"). */
#define SH_FD_CLOSED (-2)

static int unit_arith(struct sh *sh,
                      const struct sh_unit *u,
                      bool diag,
                      char *nb);
static int unit_cmdsub(struct sh *sh,
                       const struct sh_unit *u,
                       bool dq,
                       char **result);
static char *expand_word(struct sh *sh, const char *s, bool pattern, int xf);

static const char *const sh_builtin_names[] = {
    ":",    ".",      "break",   "cd",     "command", "continue", "echo",
    "eval", "exec",   "exit",    "export", "false",   "help",     "pwd",
    "read", "return", "set",     "shift",  "source",  "test",     "true",
    "type", "unset",  "waitfor", "[",      "[[",      "jobs",     "fg",
    "bg",   "trap",   NULL,
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
    ssize_t r = read(STDIN_FILENO, &c, 1);

    if (r < 0 && errno == EINTR)
        return SH_KEY_INTR;
    if (r != 1)
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
        /* ipc-send: utils ->
         * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
         */
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

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
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

/* Make a trapped signal end (restart false) or not end (true) a blocked read:
 * while the shell waits for a command line, its action has to run at once.
 */
static void sh_trap_restart(struct sh *sh, bool restart)
{
    for (int i = 1; i < SH_NTRAPS; i++) {
        struct sigaction sa;

        if (!sh->traps[i] || !*sh->traps[i] ||
            sigaction(sh_trap_sigs[i].sig, NULL, &sa) != 0)
            continue;
        sa.sa_flags =
            restart ? sa.sa_flags | SA_RESTART : sa.sa_flags & ~SA_RESTART;
        sigaction(sh_trap_sigs[i].sig, &sa, NULL);
    }
}

/* Run the pending trap actions from inside a read. They are commands like any
 * other, so a signal that arrives while one runs must not end its waits:
 * restart is back on for them, and off again for the read, for the traps they
 * set too.
 */
static void sh_run_traps_in_read(struct sh *sh)
{
    if (!sh_traps_pending())
        return;
    sh_trap_restart(sh, true);
    sh_run_traps(sh);
    sh_trap_restart(sh, false);
}

/* Run the pending trap actions while the line editor has the terminal raw: with
 * the saved modes back, so their output and the commands they run see a normal
 * terminal, and raw again for the editor.
 */
static void sh_run_traps_on_tty(struct sh *sh, const struct termios *saved)
{
    struct termios raw_again;

    tcsetattr(STDIN_FILENO, TCSANOW, saved);
    sh_write_all("\r\n");
    sh_run_traps_in_read(sh);
    (void) sh_enable_raw(&raw_again);
}

/* Trap i has an action to run: neither the default nor ignored. */
static bool trap_armed(const struct sh *sh, int i)
{
    return sh->traps[i] && *sh->traps[i];
}

/* Wait for input on stdin without losing a trapped signal that arrives between
 * the check for one that is pending and the read: those signals are blocked
 * while it is made, and unblocked only inside pselect, which then returns at
 * once if one is caught.
 *
 * Returns true when stdin can be read (or has failed, which read reports),
 * false when a signal ended the wait, with its action pending.
 */
static bool sh_await_input(struct sh *sh)
{
    sigset_t block;
    sigset_t old;
    fd_set rd;
    bool armed = false;
    int r;

    sigemptyset(&block);
    for (int i = 1; i < SH_NTRAPS; i++) {
        if (trap_armed(sh, i)) {
            sigaddset(&block, sh_trap_sigs[i].sig);
            armed = true;
        }
    }
    if (!armed)
        return true;
    sigprocmask(SIG_BLOCK, &block, &old);
    if (sh_traps_pending()) {
        sigprocmask(SIG_SETMASK, &old, NULL);
        return false;
    }
#ifdef SH_TEST_AWAIT_WINDOW
    SH_TEST_AWAIT_WINDOW(); /* where a host test sends its signal */
#endif
    FD_ZERO(&rd);
    FD_SET(STDIN_FILENO, &rd);
    r = pselect(STDIN_FILENO + 1, &rd, NULL, NULL, NULL, &old);
    sigprocmask(SIG_SETMASK, &old, NULL);
    return r >= 0 || errno != EINTR;
}

static int sh_read_line_body(struct sh *sh,
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
        /* Not a terminal: one line, a byte at a time. A signal that is trapped
         * ends the read, runs its action, and the read goes on.
         */
        fputs(prompt, stdout);
        fflush(stdout);
        while (len + 1 < outsz) {
            char ch;
            ssize_t r;

            /* A signal caught since the last read has not interrupted one. */
            sh_run_traps_in_read(sh);
            if (!sh_await_input(sh))
                continue;
            r = read(STDIN_FILENO, &ch, 1);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            out[len++] = ch;
            if (ch == '\n')
                break;
        }
        out[len] = '\0';
        return len == 0 ? -1 : 0;
    }
    buf[0] = '\0';
    sh_write_all(prompt);
    for (;;) {
        int c;

        if (sh_traps_pending()) { /* caught since the last key */
            sh_run_traps_on_tty(sh, &saved);
            sh_refresh_line(prompt, buf, pos);
        }
        if (!sh_await_input(sh))
            continue; /* a trapped signal came in: run it first */
        c = sh_read_key();
        if (c == SH_KEY_INTR) { /* a trapped signal: run it, then go on */
            sh_run_traps_on_tty(sh, &saved);
            sh_refresh_line(prompt, buf, pos);
            continue;
        }
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
                     s[n - 1] == '\t')) {
        size_t bs = 0;

        while (bs < n - 1 && s[n - 2 - bs] == '\\')
            bs++;
        if (bs % 2 && s[n - 1] != '\n' && s[n - 1] != '\r')
            break; /* a blank that a backslash makes part of the word */
        s[--n] = '\0';
    }
}

/* Whether the unquoted # at p in line starts a comment: it starts a word. */
static bool is_comment_start(const char *line, const char *p)
{
    return p == line || strchr(" \t;&|<>()", p[-1]) != NULL;
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

/* Append c, which is quoted in the word being built. */
static int append_quoted_char(char **buf, size_t *len, size_t *cap, int c)
{
    if (c != '\0' && strchr(SH_GLOB_CHARS, c) &&
        append_char(buf, len, cap, SH_QUOTED_GLOB) != 0)
        return -1;
    return append_char(buf, len, cap, c);
}

/* Append s, all of it quoted in the word being built. */
static int append_quoted_str(char **buf,
                             size_t *len,
                             size_t *cap,
                             const char *s)
{
    for (; *s != '\0'; s++)
        if (append_quoted_char(buf, len, cap, *s) != 0)
            return -1;
    return 0;
}

/* Mark as quoted what an expansion put in *buf from offset from on. */
static int mark_quoted(char **buf, size_t *len, size_t *cap, size_t from)
{
    char *tail = *buf ? strdup(*buf + from) : NULL;
    int rc;

    if (!*buf)
        return 0; /* it put nothing there */
    if (!tail)
        return -1;
    (*buf)[from] = '\0';
    *len = from;
    rc = append_quoted_str(buf, len, cap, tail);
    free(tail);
    return rc;
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

/* IFS, the characters that separate fields: its value, or blank, tab and
 * newline when it is unset. An empty IFS splits nothing.
 */
static const char *ifs_value(void)
{
    const char *v = getenv("IFS");

    return v ? v : " \t\n";
}

static bool ifs_blank_char(int c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

/* Whether c is an IFS white space character: a blank, tab or newline in IFS,
 * which runs together and is trimmed from the ends of the fields.
 */
static bool ifs_is_space(const char *ifs, int c)
{
    return ifs_blank_char(c) && strchr(ifs, c) != NULL;
}

/* The character that joins the parameters in "$*": the first of IFS, a blank
 * when it is unset, and none when it is empty (0).
 */
static char ifs_joiner(void)
{
    return ifs_value()[0];
}

/* Join $1..$n with the joiner of "$*" into a fresh buffer ("" when empty). */
static char *sh_join_params(struct sh *sh)
{
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    const char joiner = ifs_joiner();

    for (int i = 0; i < sh->nparams; i++) {
        if ((i > 0 && joiner && append_char(&buf, &len, &cap, joiner) != 0) ||
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

/* s with a mark before each byte that equals the mark, so that a field-split
 * result tells its own marks from the data. Frees s.
 */
static char *mark_literal(char *s)
{
    size_t n = strlen(s);
    size_t extra = 0;
    char *r;
    char *w;

    for (size_t i = 0; i < n; i++)
        extra += s[i] == SH_QUOTED_BLANK;
    if (extra == 0)
        return s;
    r = malloc(n + extra + 1);
    if (!r) {
        free(s);
        return NULL;
    }
    for (w = r, n = 0; s[n] != '\0'; n++) {
        if (s[n] == SH_QUOTED_BLANK)
            *w++ = SH_QUOTED_BLANK;
        *w++ = s[n];
    }
    *w = '\0';
    free(s);
    return r;
}

static int expand_dollar(struct sh *sh,
                         const char **pp,
                         char **buf,
                         size_t *len,
                         size_t *cap,
                         int xf);

/* Parse and expand ${ ... }. On entry *pp points just past '{'; on return just
 * past the matching '}'. Supports length (${#name}), ${@}/${*},
 * defaults/assign/ error/alt (${n:-w} ${n-w} ${n:=w} ${n=w} ${n:?w} ${n?w}
 * ${n:+w} ${n+w}), and prefix/suffix trims (${n#pat} ${n##pat} ${n%pat}
 * ${n%%pat}). The word of a default or alternative expands in turn, and a
 * pattern keeps its quoting.
 *
 * Returns malloc'd; "" on error.
 */
static char *expand_braced(struct sh *sh, const char **pp, int xf)
{
    struct sh_scan sc = {*pp - 2, 0}; /* at the $ of ${ */
    struct sh_unit u;
    char *content;
    char *name;
    const char *op;
    const char *value;
    size_t nlen;
    int colon = 0;
    bool from_word = false; /* result is a word expand_word marked */
    char *result;

    sh_unit_next(&sc, &u);
    if (u.kind != SH_U_BRACE || !u.closed)
        return strdup("");
    content = dup_range(u.body, u.body_len);
    if (!content)
        return strdup("");
    *pp = sc.p;

    /* ${#} ${?} ${$} ${!} ${-}: the special parameters, as without braces. */
    if (content[0] != '\0' && content[1] == '\0' &&
        strchr("#?$!-", content[0])) {
        const char *q = content;
        char *val = NULL;
        size_t vlen = 0;
        size_t vcap = 0;
        int rc = expand_dollar(sh, &q, &val, &vlen, &vcap, 0);

        free(content);
        if (rc != 0) {
            free(val);
            return strdup("");
        }
        return val ? val : strdup("");
    }

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
        char *pat = expand_word(sh, op + (longest ? 2 : 1), true, 0);

        nounset_check(sh, name, value);
        result = pat ? sh_trim(cur, pat, *op == '#', longest) : strdup(cur);
        free(pat);
    } else {
        int missing;
        const char *word;
        char *wexp = NULL;
        char *wplain = NULL;

        if (*op == ':') {
            colon = 1;
            op++;
        }
        missing = value == NULL || (colon && value[0] == '\0');
        word = op[0] != '\0' ? op + 1 : "";

        /* Only the word that is used is expanded, as POSIX 2.6.2 says. */
        if ((*op == '-' && missing) || (*op == '+' && !missing))
            wexp = expand_word(sh, word, false, xf);
        else if ((*op == '=' && missing) || (*op == '?' && missing))
            wplain = expand_word(sh, word, false, xf & XF_DQ);
        switch (*op) {
        case '-':
            result = missing ? wexp : strdup(cur);
            from_word = missing;
            break;
        case '=':
            if (!missing)
                result = strdup(cur);
            else if (!wplain)
                result = NULL;
            else if (!is_valid_varname(name))
                result = braced_fail(sh, name, "cannot assign in this way");
            else if (setenv(name, wplain, 1) != 0)
                result = braced_fail(sh, name, "assignment failed");
            else
                result = strdup(wplain);
            break;
        case '?':
            if (missing)
                result = braced_fail(
                    sh, name,
                    wplain && wplain[0] ? wplain : "parameter null or not set");
            else
                result = strdup(cur);
            break;
        case '+':
            result = missing ? strdup("") : wexp;
            from_word = !missing;
            break;
        default:
            result = strdup(cur);
            break;
        }
        free(wplain);
    }
    free(name);
    free(content);
    if ((xf & XF_SPLIT) && !from_word && result)
        result = mark_literal(result);
    return result;
}

static int expand_dollar(struct sh *sh,
                         const char **pp,
                         char **buf,
                         size_t *len,
                         size_t *cap,
                         int xf)
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
        owned = expand_braced(sh, &p, xf);
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
    if (*p == '$') { /* $$ -> shell process id, a subshell's parent's */
        snprintf(tmp, sizeof(tmp), "%ld", (long) sh_pid());
        *pp = p + 1;
        return append_str(buf, len, cap, tmp);
    }
    if (*p == '!') { /* $! -> last background process, empty if none */
        if (sh_last_bg)
            snprintf(tmp, sizeof(tmp), "%ld", (long) sh_last_bg);
        else
            tmp[0] = '\0';
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
    if (*p == '@' || *p == '*') { /* join $1..$n; tokenize splits "$@" */
        char sep = *p == '@' ? ' ' : ifs_joiner();

        *pp = p + 1;
        for (int i = 0; i < sh->nparams; i++) {
            if (i > 0 && sep && append_char(buf, len, cap, sep) != 0)
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

static int sh_spawn_sub(struct sh *sh,
                        char kind,
                        char **lines,
                        int nlines,
                        int in_fd,
                        int out_fd,
                        int err_fd,
                        int background,
                        pid_t *pid_out,
                        const char *label);

/* Read fd to end of file into a malloc'd, NUL-terminated *out (its length,
 * without the NUL, in *lenp).
 *
 * Returns 0, or -1 on a read or allocation failure (*out is then NULL).
 */
static int read_all(int fd, char **out, size_t *lenp)
{
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    for (;;) {
        ssize_t r;

        if (cap - len < 512) {
            char *nb = realloc(buf, cap = cap ? cap * 2 : 1024);

            if (!nb)
                break;
            buf = nb;
        }
        r = read(fd, buf + len, cap - len - 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            break;
        if (r == 0) {
            buf[len] = '\0';
            *out = buf;
            *lenp = len;
            return 0;
        }
        len += (size_t) r;
    }
    free(buf);
    *out = NULL;
    return -1;
}

/* Run `inner` as a command substitution and capture its stdout into *out
 * (malloc'd, trailing newlines stripped). Runs it in a child shell that gets
 * this shell's state (sh_spawn_sub), and drains the pipe concurrently, so there
 * is no PIPE_BUF deadlock and output is unbounded.
 *
 * Returns 0, or -1 on failure (with *out set to "").
 */
static int capture_command(struct sh *sh, const char *inner, char **out)
{
    int pfd[2];
    pid_t pid = -1;
    char *buf;
    size_t len;
    int status;
    int ws;
    pid_t reaped;
    bool jc = sh_jc;

    *out = strdup("");
    if (!*out)
        return -1;
    if (pipe(pfd) != 0) {
        fprintf(stderr, "sh: command substitution: %s\n", strerror(errno));
        return -1;
    }
    (void) fcntl(pfd[0], F_SETFD, FD_CLOEXEC);

    /* The substitution runs in the shell's own process group, not a job of its
     * own, so it never takes the terminal.
     */
    sh_jc = false;
    status = sh_spawn_sub(sh, 'c', (char **) &inner, 1, -1, pfd[1], -1, 0, &pid,
                          "sh");
    sh_jc = jc;
    close(pfd[1]);
    if (status != 0 || pid <= 0) {
        close(pfd[0]);
        if (pid > 0) /* started, but its state could not be sent */
            (void) waitpid(pid, NULL, 0);
        return -1;
    }
    status = read_all(pfd[0], &buf, &len);
    close(pfd[0]);
    while ((reaped = waitpid(pid, &ws, 0)) < 0 && errno == EINTR)
        ;
    sh->subst_status = reaped < 0        ? 1
                       : WIFEXITED(ws)   ? WEXITSTATUS(ws)
                       : WIFSIGNALED(ws) ? 128 + WTERMSIG(ws)
                                         : 1;
    if (status != 0)
        return -1;
    while (len > 0 && buf[len - 1] == '\n')
        buf[--len] = '\0';
    free(*out);
    *out = buf;
    return 0;
}

static int emit_field(struct sh_token *tok,
                      int *ntok,
                      const char *s,
                      bool glob);

/* Emit an expansion result into the word being built. Quoted: append it
 * literally. Unquoted: split it at the characters of IFS (POSIX 2.6.5). A run
 * of IFS white space ends the word built so far, if it has any content, as a
 * token of its own; so does each other IFS character, and two of these with
 * nothing between them make an empty field. A white space run beside one of
 * these belongs to it. *produced tells whether the word being built has
 * content, so the caller can drop a word that is nothing but an empty unquoted
 * expansion. *glob tells whether the field being built has an unquoted * ? or [
 * in it, which makes the finished field a pattern for pathname expansion (POSIX
 * 2.6.6 applies it to the result of an expansion).
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
                           bool marks,
                           int *produced,
                           int *glob)
{
    const char *ifs = ifs_value();
    enum { CONTENT, SPACE_ENDED, SPACE_SKIPPED, DELIM } last = CONTENT;

    if (quote) { /* double quotes: a field, even if empty */
        *produced = 1;
        return append_quoted_str(buf, len, cap, text);
    }
    for (const char *p = text; *p != '\0'; p++) {
        bool quoted_char = false;

        if (marks && *p == SH_QUOTED_BLANK && p[1] == SH_QUOTED_EMPTY) {
            p++; /* an empty quoted string: a field, if nothing else */
            last = CONTENT;
            *produced = 1;
            continue;
        }
        if (marks && *p == SH_QUOTED_BLANK && p[1] != '\0') {
            p++; /* quoted in a ${...} word: no split, and no longer a mark */
            quoted_char = true;
        } else if (strchr(ifs, *p) != NULL) {
            /* strchr above already found *p in IFS. */
            bool space = ifs_blank_char((unsigned char) *p);

            if (space) {
                /* A white space run: the first ends the word if it has content,
                 * and the rest, like leading white space, is skipped.
                 */
                if (last != CONTENT)
                    continue;
                if (!(*produced || (*buf && **buf))) {
                    last = SPACE_SKIPPED;
                    continue;
                }
                last = SPACE_ENDED;
            } else if (last == SPACE_ENDED) {
                last = DELIM; /* it ends the field the white space ended */
                continue;
            } else {
                last = DELIM;
            }
            if (emit_field(tok, ntok, *buf ? *buf : "", *glob) != 0)
                return -1;
            free(*buf);
            *buf = NULL;
            *len = 0;
            *cap = 0;
            *produced = 0;
            *glob = 0;
            continue;
        }
        last = CONTENT;
        if (!quoted_char && strchr("*?[", *p))
            *glob = 1;
        if ((quoted_char ? append_quoted_char(buf, len, cap, *p)
                         : append_char(buf, len, cap, *p)) != 0)
            return -1;
        *produced = 1;
    }
    return 0;
}

/* The command text of a $( ... ) or ` ... ` unit, malloc'd. A backtick body
 * loses the backslash before ` \ and $, and before " inside double quotes.
 */
static char *sub_text(const struct sh_unit *u, bool dq)
{
    char *r = dup_range(u->body, u->body_len);
    char *w = r;

    if (!r || u->kind != SH_U_BQ)
        return r;
    for (const char *c = r; *c != '\0'; c++) {
        if (*c == '\\' &&
            (c[1] == '`' || c[1] == '\\' || c[1] == '$' || (dq && c[1] == '"')))
            c++;
        *w++ = *c;
    }
    *w = '\0';
    return r;
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
    while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n')
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

/* Evaluate the $(( ... )) unit u into nb (32 bytes). A malformed expression is
 * 0, with a diagnostic when diag is set.
 *
 * Returns 0, or -1 on allocation failure.
 */
static int unit_arith(struct sh *sh,
                      const struct sh_unit *u,
                      bool diag,
                      char *nb)
{
    char *expr = dup_range(u->body, u->body_len);
    struct arith_ctx a;
    long val;

    if (!expr)
        return -1;
    a.p = expr;
    a.sh = sh;
    a.err = 0;
    a.dead = 0;
    a.depth = 0;
    val = arith_lor(&a);
    arith_skip(&a);
    if (!u->closed || a.err || *a.p != '\0') {
        if (diag)
            fprintf(stderr, "sh: arithmetic error: %s\n", expr);
        val = 0;
    }
    free(expr);
    snprintf(nb, 32, "%ld", val);
    return 0;
}

/* Run the $( ... ) or ` ... ` unit u as a command substitution into *result
 * (malloc'd, trailing newlines stripped).
 *
 * Returns 0, or -1 on failure.
 */
static int unit_cmdsub(struct sh *sh,
                       const struct sh_unit *u,
                       bool dq,
                       char **result)
{
    char *inner = sub_text(u, dq);
    int rc;

    if (!inner)
        return -1;
    rc = capture_command(sh, inner, result);
    free(inner);
    if (rc != 0) {
        free(*result);
        *result =
            NULL; /* a caller that frees its copy must not free it twice */
    }
    return rc;
}

/* Expand the text of a ${name op word} word or of a trim pattern: quote
 * removal, and parameter, command and arithmetic expansion. A pattern keeps the
 * glob characters it quoted literal with a backslash. A word whose result is
 * field-split (XF_SPLIT) marks its quoted blanks. In double quotes (XF_DQ) a
 * word's single quotes are literal, as the reference shell has it.
 *
 * Returns the malloc'd result, or NULL on failure.
 */
static char *expand_word(struct sh *sh, const char *s, bool pattern, int xf)
{
    struct sh_scan sc = {s, 0};
    struct sh_unit u;
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    for (;;) {
        const char *at = sc.p;
        char *val = NULL;
        size_t vlen = 0;
        size_t vcap = 0;
        char nb[32];
        const char *t = NULL; /* text to add, quoted when sc.quote or ESC */
        const char *ifs;
        bool quoted;
        bool premarked = false;
        int rc = 0;

        sh_unit_next(&sc, &u);
        quoted = sc.quote != 0 || u.kind == SH_U_ESC;
        switch (u.kind) {
        case SH_U_END:
            return buf ? buf : strdup("");
        case SH_U_QOPEN:
            if (*at == '\'' && (xf & XF_DQ) && !pattern) {
                sc.quote = 0; /* a literal ' */
                rc = append_char(&buf, &len, &cap, '\'');
            } else if ((xf & XF_SPLIT) && !pattern) {
                rc = append_char(&buf, &len, &cap, SH_QUOTED_BLANK);
                if (rc == 0)
                    rc = append_char(&buf, &len, &cap, SH_QUOTED_EMPTY);
            }
            break;
        case SH_U_QCLOSE:
            break;
        case SH_U_ESC:
            nb[0] = at[1];
            nb[1] = '\0';
            t = nb;
            break;
        case SH_U_LIT:
            nb[0] = *at;
            nb[1] = '\0';
            t = nb;
            break;
        case SH_U_ARITH:
            rc = unit_arith(sh, &u, true, nb);
            t = nb;
            break;
        case SH_U_PAREN:
        case SH_U_BQ:
            rc = unit_cmdsub(sh, &u, sc.quote == '"', &val);
            t = val;
            break;
        case SH_U_BRACE:
        case SH_U_DOLLAR:
            at++;

            /* An unquoted ${...} in a word that is itself field-split keeps the
             * marks it made, which the loop below must not mark again.
             */
            premarked =
                u.kind == SH_U_BRACE && !quoted && !pattern && (xf & XF_SPLIT);
            rc = expand_dollar(sh, &at, &val, &vlen, &vcap,
                               quoted      ? XF_DQ
                               : premarked ? XF_SPLIT
                                           : 0);
            if (u.kind == SH_U_DOLLAR) /* a brace unit was consumed whole */
                sc.p = at;
            t = val;
            break;
        }
        ifs = ifs_value(); /* after the unit: ${IFS=...} may set it */
        for (; rc == 0 && t && *t; t++) {
            if (quoted && pattern && strchr("*?[]\\!", *t))
                rc = append_char(&buf, &len, &cap, '\\');
            else if ((xf & XF_SPLIT) && !pattern && !premarked &&
                     (*t == SH_QUOTED_BLANK ||
                      (quoted && (strchr(ifs, *t) || strchr("*?[", *t)))))
                rc = append_char(&buf, &len, &cap, SH_QUOTED_BLANK);
            if (rc == 0)
                rc = append_char(&buf, &len, &cap, *t);
        }
        free(val);
        if (rc != 0) {
            free(buf);
            return NULL;
        }
    }
}

static int glob_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/* Remove the backslash before each character of a pattern's directory part. */
static void glob_unescape(char *s)
{
    char *w = s;

    for (; *s != '\0'; s++) {
        if (*s == '\\' && s[1] != '\0')
            s++;
        *w++ = *s;
    }
    *w = '\0';
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
        glob_unescape(prefix); /* the directories are named, not matched */
        glob_unescape(diropen);
    } else {
        prefix[0] = '\0';
        filepat = pat;
        diropen[0] = '.';
        diropen[1] = '\0';
    }

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
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

/* Add field s as a word, or the sorted names it matches if glob says it is a
 * pattern (a pattern that matches nothing stays a word). The characters
 * SH_QUOTED_GLOB marks match only themselves.
 *
 * Returns 0, or -1 on allocation failure.
 */
static int emit_field(struct sh_token *tok, int *ntok, const char *s, bool glob)
{
    size_t n = strlen(s);
    char *lit;
    char *pat;
    char *l;
    char *w;
    int rc = -1;

    if (!glob && !strchr(s, SH_QUOTED_GLOB))
        return token_add(tok, ntok, SH_WORD, s); /* nothing to strip */
    lit = malloc(n + 1);
    pat = glob ? malloc(2 * n + 1) : NULL;
    l = lit;
    w = pat;
    if (!lit || (glob && !pat))
        goto out;
    for (const char *c = s; *c != '\0'; c++) {
        bool quoted =
            *c == SH_QUOTED_GLOB && c[1] != '\0' && strchr(SH_GLOB_CHARS, c[1]);

        c += quoted;
        *l++ = *c;
        if (w) {
            if (quoted)
                *w++ = '\\';
            *w++ = *c;
        }
    }
    *l = '\0';
    rc = 0;
    if (glob && *lit != '\0') {
        struct sh_words g = {0};
        int m;

        *w = '\0';
        m = glob_word(pat, &g);
        if (m < 0)
            rc = -1;
        for (int i = 0; i < g.n && rc == 0; i++)
            rc = token_add(tok, ntok, SH_WORD, g.v[i]);
        words_free(&g);
        if (m != 0)
            goto out;
    }
    rc = token_add(tok, ntok, SH_WORD, lit);
out:
    free(lit);
    free(pat);
    return rc;
}

static bool is_redir_op(enum sh_op op)
{
    return op == SH_LT || op == SH_GT || op == SH_GTGT || op == SH_ERRGT ||
           op == SH_ERRGTGT || op == SH_DUPOUT || op == SH_DUPERR;
}

static bool is_assignment(const char *s);

/* Whether the word at p, after tok[0,ntok) of its line, is expanded without
 * field splitting or pathname expansion: NAME=value before the command name or
 * as an argument of export or readonly (POSIX 2.9.1), or the subject word of
 * `case`.
 */
static bool word_is_unsplit(const struct sh_token *tok, int ntok, const char *p)
{
    const char *cmd = NULL;
    int i = ntok;

    if (ntok > 0 && is_redir_op(tok[ntok - 1].op))
        return false; /* a redirection target */
    while (i > 0 && (tok[i - 1].op == SH_WORD || is_redir_op(tok[i - 1].op)))
        i--;
    for (; i < ntok && !cmd; i++) {
        if (is_redir_op(tok[i].op))
            i++; /* and its target */
        else if (!is_assignment(tok[i].s))
            cmd = tok[i].s;
    }
    if (cmd && cmd == tok[ntok - 1].s && !strcmp(cmd, "case"))
        return true;
    return is_assignment(p) &&
           (!cmd || !strcmp(cmd, "export") || !strcmp(cmd, "readonly"));
}

/* Length of the $@, $*, ${@} or ${*} at p, which points at a `$` (0 if it is
 * none of these); *which gets the @ or *.
 */
static size_t all_params_at(const char *p, char *which)
{
    bool braced = p[1] == '{';
    const char *c = p + 1 + braced;

    if ((*c != '@' && *c != '*') || (braced && c[1] != '}'))
        return 0;
    *which = *c;
    return braced ? 4 : 2;
}

/* What is left open at the end of line, a quote or a substitution that never
 * ends, as the text of a diagnostic, or NULL when all is closed. A comment is
 * not read.
 */
static const char *line_unfinished(const char *line)
{
    struct sh_scan sc = {line, 0};
    struct sh_unit u;

    for (;;) {
        const char *p = sc.p;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            break;
        if (!u.closed)
            return u.kind == SH_U_BRACE   ? "missing }"
                   : u.kind == SH_U_BQ    ? "unterminated `"
                   : u.kind == SH_U_ARITH ? "unterminated $(("
                                          : "unterminated $(";
        if (u.kind == SH_U_LIT && sc.quote == 0 && *p == '#' &&
            is_comment_start(line, p))
            return NULL;
    }
    return sc.quote != 0 ? "unterminated quoted string" : NULL;
}

/* Whether line, which ends inside a quote or a substitution, goes on in the
 * next line, and how they join: *cut is the length of line to keep and *blank
 * whether a newline goes between. A backslash that ends a line inside double
 * quotes takes the newline with it.
 */
static bool line_open_join(const char *line, size_t *cut, bool *blank)
{
    struct sh_scan sc = {line, 0};
    struct sh_unit u;
    bool lone = false; /* the last unit is a backslash with nothing after it */

    if (!strpbrk(line, "\"'`$") || !line_unfinished(line))
        return false;
    for (;;) {
        const char *p = sc.p;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            break;
        lone = u.kind == SH_U_LIT && *p == '\\' && p[1] == '\0';
    }
    *cut = strlen(line);
    *blank = true;
    if (lone && sc.quote == '"') {
        --*cut;
        *blank = false;
    }
    return true;
}

/* Whether line is complete, as a POSIX shell takes a quote or a substitution
 * that never ends for a syntax error before running any of the line. On failure
 * the error is reported, the status is 2 and a shell that is not interactive
 * ends.
 */
static bool line_is_complete(struct sh *sh, const char *line)
{
    const char *why = line_unfinished(line);

    if (!why)
        return true;
    fprintf(stderr, "sh: syntax error: %s\n", why);
    sh->last_status = 2;
    if (!sh->interactive)
        sh->exit_requested = true;
    return false;
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
        int word_may_glob = 0; /* has an unquoted * ? [ metacharacter */
        int at_word_start = 1; /* still at the unquoted word start */
        bool unsplit;          /* see word_is_unsplit */
        int dq_content = 0;    /* word_has_content before the quote */
        int dq_bare =
            0; /* in "...": 1 empty so far, 2 only bare "$@", else 0 */
        struct sh_scan sc;
        struct sh_unit u;

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

        /* [1|2]> and [1|2]>>, and [1|2]>&WORD, which duplicates the fd WORD
         * expands to, or closes the fd for -: an SH_DUPOUT or SH_DUPERR whose
         * target is the word that follows.
         */
        if (*p == '>' || ((*p == '1' || *p == '2') && p[1] == '>')) {
            bool err = *p == '2';
            bool app;
            bool dup;

            p += *p != '>';
            app = p[1] == '>';
            dup = !app && p[1] == '&';
            if (token_add(tok, &ntok,
                          dup   ? (err ? SH_DUPERR : SH_DUPOUT)
                          : err ? (app ? SH_ERRGTGT : SH_ERRGT)
                                : (app ? SH_GTGT : SH_GT),
                          NULL) != 0)
                goto fail;
            p += 1 + app + dup;
            continue;
        }
        unsplit = word_is_unsplit(tok, ntok, p);
        while (*p != '\0') {
            if (quote == 0 &&
                (*p == ' ' || *p == '\t' || *p == ';' || *p == '&' ||
                 *p == '|' || *p == '<' || *p == '>'))
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
            sc.p = p;
            sc.quote = quote;
            sh_unit_next(&sc, &u);
            if (u.kind == SH_U_QOPEN || u.kind == SH_U_QCLOSE) {
                if (u.kind == SH_U_QOPEN) {
                    dq_content = word_has_content;
                    dq_bare = *p == '"';
                }
                quote = sc.quote;
                p++;

                /* A "..." of nothing but "$@" with no parameters is no field,
                 * not even an empty one.
                 */
                word_has_content =
                    u.kind == SH_U_QCLOSE && dq_bare == 2 ? dq_content : 1;
                continue;
            }
            if (quote == '"') {
                char which;
                bool bare = (u.kind == SH_U_DOLLAR || u.kind == SH_U_BRACE) &&
                            all_params_at(p, &which) && which == '@' &&
                            sh->nparams == 0;

                dq_bare = bare && dq_bare ? 2 : 0;
            }

            /* An escaped character is literal; a quoted glob metacharacter
             * stays literal too.
             */
            if (u.kind == SH_U_ESC) {
                p++;
                word_has_content = 1;
                if (append_quoted_char(&buf, &len, &cap, *p++) != 0) {
                    free(buf);
                    goto fail;
                }
                continue;
            }
            if (u.kind == SH_U_ARITH) {
                char nb[32];

                p += u.len;
                if (unit_arith(sh, &u, true, nb) != 0) {
                    free(buf);
                    goto fail;
                }
                word_has_content = 1;
                if (append_str(&buf, &len, &cap, nb) != 0) {
                    free(buf);
                    goto fail;
                }
                continue;
            }
            if (u.kind == SH_U_PAREN || u.kind == SH_U_BQ) {
                char *result;

                p += u.len;
                if (unit_cmdsub(sh, &u, quote == '"', &result) != 0) {
                    free(buf);
                    goto fail;
                }
                if (emit_sub_result(tok, &ntok, &buf, &len, &cap,
                                    quote || unsplit, result, false,
                                    &word_has_content, &word_may_glob) != 0) {
                    free(result);
                    free(buf);
                    goto fail;
                }
                free(result);
                continue;
            }
            if (u.kind == SH_U_DOLLAR || u.kind == SH_U_BRACE) {
                char *val = NULL;
                size_t vlen = 0;
                size_t vcap = 0;
                char which;
                size_t n = all_params_at(p, &which);
                int rc;

                /* "$@": a field per positional parameter, the first joined to
                 * what precedes it and the last to what follows. A "$@" that is
                 * the whole quoted part and has no parameters is no field.
                 */
                if (n && which == '@' && quote == '"' && !unsplit) {
                    if (sh->nparams == 0) {
                        p += n; /* nothing, and no field of its own */
                        continue;
                    }
                    dq_bare = 0;
                    for (int k = 0; k < sh->nparams; k++) {
                        if (k > 0) {
                            if (emit_field(tok, &ntok, buf ? buf : "", false) !=
                                0) {
                                free(buf);
                                goto fail;
                            }
                            len = 0;
                            if (buf)
                                buf[0] = '\0';
                        }
                        if (append_quoted_str(&buf, &len, &cap,
                                              sh->params[k]) != 0) {
                            free(buf);
                            goto fail;
                        }
                    }
                    p += n;
                    continue;
                }

                /* Unquoted $@ and $*: each parameter is a field of its own, and
                 * is then split on IFS by itself; an empty one is no field.
                 */
                if (n && !quote && !unsplit) {
                    for (int k = 0; k < sh->nparams; k++) {
                        if (k > 0 && word_has_content) {
                            if (emit_field(tok, &ntok, buf ? buf : "",
                                           word_may_glob) != 0) {
                                free(buf);
                                goto fail;
                            }
                            free(buf);
                            buf = NULL;
                            len = cap = 0;
                            word_has_content = 0;
                            word_may_glob = 0;
                        }
                        if (emit_sub_result(tok, &ntok, &buf, &len, &cap, 0,
                                            sh->params[k], false,
                                            &word_has_content,
                                            &word_may_glob) != 0) {
                            free(buf);
                            goto fail;
                        }
                    }
                    p += n;
                    continue;
                }

                /* Anything else expands to one string, which is field-split
                 * when unquoted: an empty unquoted expansion is no field.
                 */
                p++;
                if (quote || unsplit) {
                    word_has_content = 1; /* a field, even if empty */
                    size_t from = len;

                    rc = expand_dollar(sh, &p, &buf, &len, &cap,
                                       quote ? XF_DQ : 0);
                    if (rc == 0 && quote)
                        rc = mark_quoted(&buf, &len, &cap, from);
                } else {
                    rc = expand_dollar(sh, &p, &val, &vlen, &vcap, XF_SPLIT);
                    if (rc == 0 && val)
                        rc = emit_sub_result(tok, &ntok, &buf, &len, &cap, 0,
                                             val, u.kind == SH_U_BRACE,
                                             &word_has_content, &word_may_glob);
                    free(val);
                }
                if (rc != 0) {
                    free(buf);
                    goto fail;
                }
                continue;
            }

            /* An unquoted glob metacharacter marks the word for pathname
             * expansion; a quoted one stays literal in the pattern.
             */
            if (quote == 0 && strchr("*?[", *p))
                word_may_glob = 1;
            word_has_content = 1;
            if ((quote ? append_quoted_char(&buf, &len, &cap, *p++)
                       : append_char(&buf, &len, &cap, *p++)) != 0) {
                free(buf);
                goto fail;
            }
        }
        if (word_has_content || (buf && buf[0] != '\0')) {
            if (!buf)
                buf = strdup("");
            if (!buf)
                goto fail;
            if (emit_field(tok, &ntok, buf, word_may_glob && !unsplit) != 0) {
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

    if (!s || (!isalpha((unsigned char) s[0]) && s[0] != '_'))
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
    if (n >= 2 && !strcmp(argv[off], "-s")) { /* exists and is not empty */
        struct stat st;

        return stat(argv[off + 1], &st) == 0 && st.st_size > 0 ? 0 : 1;
    }
    if (n >= 3 && !strcmp(argv[off + 1], "="))
        return !strcmp(argv[off], argv[off + 2]) ? 0 : 1;
    if (n >= 3 && !strcmp(argv[off + 1], "!="))
        return strcmp(argv[off], argv[off + 2]) != 0 ? 0 : 1;
    if (n >= 3 && argv[off + 1][0] == '-') {
        static const char *const iops[] = {"-eq", "-ne", "-lt",
                                           "-le", "-gt", "-ge"};
        char *e1;
        char *e2;
        long l = strtol(argv[off], &e1, 10);
        long r = strtol(argv[off + 2], &e2, 10);
        int k = 0;

        while (k < 6 && strcmp(argv[off + 1], iops[k]))
            k++;
        if (k < 6) {
            bool t[] = {l == r, l != r, (l < r), l <= r, (l > r), l >= r};

            if (e1 == argv[off] || *e1 || e2 == argv[off + 2] || *e2) {
                fprintf(stderr, "test: integer expression expected\n");
                return 2;
            }
            return t[k] ? 0 : 1;
        }
    }
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
    const char *ifs;
    char *p;

    if (argv[1] && !strcmp(argv[1], "-r"))
        varidx = 2;
    if (!argv[varidx])
        return 1;
    if (in_fd < 0) /* no pipe or redirection: the shell's own stdin */
        in_fd = STDIN_FILENO;
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

    /* Fields at the characters of IFS (POSIX 2.6.5); the last variable gets the
     * rest of the line, less the IFS white space at its end.
     */
    ifs = ifs_value();
    p = buf;
    for (int vi = 0; vi < nvars; vi++) {
        char *field;
        size_t fl;
        char save;
        int rc;

        while (*p != '\0' && ifs_is_space(ifs, (unsigned char) *p))
            p++;
        field = p;
        if (vi == nvars - 1) {
            fl = strlen(p);
            while (fl > 0 && ifs_is_space(ifs, (unsigned char) p[fl - 1]))
                fl--;

            /* One delimiter at the end ends the last field, as it would between
             * two; a run of them keeps its empty fields.
             */
            size_t before = fl > 0 ? fl - 1 : 0;

            while (before > 0 &&
                   ifs_is_space(ifs, (unsigned char) p[before - 1]))
                before--;
            if (fl > 0 && strchr(ifs, p[fl - 1]) &&
                (before == 0 || !strchr(ifs, p[before - 1]))) {
                fl--;
                while (fl > 0 && ifs_is_space(ifs, (unsigned char) p[fl - 1]))
                    fl--;
            }
        } else {
            while (*p != '\0' && !strchr(ifs, *p))
                p++;
            fl = (size_t) (p - field);
            if (*p != '\0' && ifs_is_space(ifs, (unsigned char) *p)) {
                while (*p != '\0' && ifs_is_space(ifs, (unsigned char) *p))
                    p++;
                if (*p != '\0' && strchr(ifs, *p))
                    p++; /* a delimiter beside white space is one with it */
            } else if (*p != '\0') {
                p++;
            }
        }
        save = field[fl];
        field[fl] = '\0';
        rc = setenv(argv[varidx + vi], field, 1);
        field[fl] = save;
        if (rc != 0)
            return 1;
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

/* interactive job control
 *
 * An interactive shell on its controlling terminal keeps itself in a process
 * group of its own and runs each pipeline in a group of its own, which it makes
 * the terminal's foreground group while the pipeline runs. ^C and ^Z then reach
 * the job and not the shell, which ignores SIGINT, SIGQUIT, SIGTSTP, SIGTTIN
 * and SIGTTOU. A stopped job stays in the table for fg/bg; a background job
 * stays there until it exits.
 */
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

/* Wait for a foreground job's last process.
 *
 * Returns its exit status as the shell reports it; *stopped is set when the job
 * stopped instead (^Z or a stop signal), in which case it is in the job table.
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
 * recent one.
 *
 * Returns NULL (after a message) when there is no such job.
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
            sh_job_report(&sh_jobs[i],
                          sh_jobs[i].stopped ? "Stopped" : "Running");
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

/* Signals caught for a trap, run by sh_run_traps between commands. */
static volatile sig_atomic_t sh_trap_pending[SH_NTRAPS];

static void sh_trap_handler(int sig)
{
    for (int i = 1; i < SH_NTRAPS; i++)
        if (sh_trap_sigs[i].sig == sig)
            sh_trap_pending[i] = 1;
}

/* The sh_trap_sigs index a trap condition names (EXIT, INT, SIGINT, 2...), or
 * -1.
 */
static int sh_trap_index(const char *s)
{
    char *end;
    long n = strtol(s, &end, 10);

    if (!strncmp(s, "SIG", 3))
        s += 3;
    for (int i = 0; i < SH_NTRAPS; i++)
        if (!strcmp(s, sh_trap_sigs[i].name) ||
            (*end == '\0' && end != s && n == sh_trap_sigs[i].sig))
            return i;
    return -1;
}

/* Set trap i to action (NULL: the default, "": ignored), taking it over. */
static void sh_set_trap(struct sh *sh, int i, char *action)
{
    struct sigaction sa = {0};

    free(sh->traps[i]);
    sh->traps[i] = action;
    sh->trap_inherited[i] = false;
    if (i == 0)
        return;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (!action)
        /* An interactive shell ignores ^C itself (run_interactive). */
        sa.sa_handler = sh->interactive && sh_trap_sigs[i].sig == SIGINT
                            ? SIG_IGN
                            : SIG_DFL;
    else
        sa.sa_handler = *action ? sh_trap_handler : SIG_IGN;
    sigaction(sh_trap_sigs[i].sig, &sa, NULL);
}

/* trap [-- ] [ACTION CONDITION...]: with no operands, list the traps in a form
 * the shell can read back; ACTION `-` resets a condition, "" ignores it. A
 * first operand that is a number is a condition, so every operand is reset.
 */
static int builtin_trap(struct sh *sh, int argc, char **argv)
{
    int a = 1;
    int rc = 0;
    const char *action;

    if (a < argc && !strcmp(argv[a], "--"))
        a++;
    if (a == argc) {
        for (int i = 0; i < SH_NTRAPS; i++) {
            if (!sh->traps[i])
                continue;
            fputs("trap -- '", stdout);
            for (const char *c = sh->traps[i]; *c; c++)
                fputs(*c == '\'' ? "'\\''" : (char[2]) {*c, '\0'}, stdout);
            printf("' %s\n", sh_trap_sigs[i].name);
        }
        return 0;
    }
    action = argv[a];
    if ((*action && action[strspn(action, "0123456789")] == '\0') ||
        a + 1 == argc)
        action = "-"; /* `trap 2` or `trap INT`: reset */
    else
        a++;
    for (; a < argc; a++) {
        int i = sh_trap_index(argv[a]);
        char *copy = NULL;

        if (i < 0) {
            fprintf(stderr, "trap: %s: bad trap\n", argv[a]);
            rc = 1;
            continue;
        }
        if (strcmp(action, "-") && !(copy = strdup(action))) {
            rc = 1;
            continue;
        }
        sh_set_trap(sh, i, copy);
    }
    return rc;
}

/* Whether a trapped signal was caught and its action has not run. */
static bool sh_traps_pending(void)
{
    for (int i = 1; i < SH_NTRAPS; i++)
        if (sh_trap_pending[i])
            return true;
    return false;
}

/* Run the actions of the signals caught since the last call. A trap action does
 * not change $?.
 */
static void sh_run_traps(struct sh *sh)
{
    static bool running;

    if (running)
        return;
    running = true;
    for (int i = 1; i < SH_NTRAPS; i++) {
        int status = sh->last_status;

        if (!sh_trap_pending[i])
            continue;
        sh_trap_pending[i] = 0;
        if (trap_armed(sh, i) && !sh->trap_inherited[i]) {
            (void) run_text(sh, sh->traps[i]);
            if (!sh->exit_requested)
                sh->last_status = status;
        }
    }
    running = false;
}

/* At exit, run the EXIT trap, once, with $? the exit status.
 *
 * Returns the status to exit with: the trap's if it ran `exit`.
 */
static int sh_exit_trap(struct sh *sh, int status)
{
    char *action = sh->traps[0];
    int rc;

    if (!action || sh->trap_inherited[0]) /* not this shell's own */
        return status;
    sh->traps[0] = NULL;
    sh->last_status = status;
    sh->exit_requested = false;
    sh->return_requested = false;
    sh->loop_break = sh->loop_continue = 0;
    rc = run_text(sh, action);
    free(action);
    return sh->exit_requested ? rc : status;
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
        for (int i = 1; argv[i]; i++)
            if (is_assignment(argv[i]) && sh_assign(argv[i], NULL) != 0)
                return 1;
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
            "exec test trap waitfor help");
        puts(
            "editing: arrows/home/end, backspace/delete, Ctrl-A/E/U/K/L/D, "
            "Tab");
        return 0;
    }
    if (!strcmp(argv[0], "read"))
        return builtin_read(argv, in_fd);
    if (!strcmp(argv[0], "waitfor"))
        return builtin_waitfor(argc, argv);
    if (!strcmp(argv[0], "trap"))
        return builtin_trap(sh, argc, argv);
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
        rc = joined ? run_text(sh, joined) : 0;
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

    while ((pid = waitpid(-1, &st, sh_jc ? WNOHANG | WUNTRACED : WNOHANG)) >
           0) {
        struct sh_job *j = sh_job_find(pid);

        if (!j)
            continue;
        if (WIFSTOPPED(st)) {
            j->stopped = true;
            sh_job_report(j, "Stopped");
        } else {
            if (sh_jc)
                sh_job_report(j, "Done");
            j->pid = 0;
        }
    }
}

/* Spawn argv with the given fds. If pid_out is non-NULL, the pid is returned
 * via *pid_out and the function returns 0 without waiting (used to assemble a
 * pipeline that is waited on collectively). Otherwise, the call blocks on
 * waitpid(). background 1 starts a job named `label` (NULL: argv[0]); 2 is an
 * earlier stage of a background pipeline, which belongs to the job its last
 * stage starts. close_fd, when not -1, is an fd the child must not inherit.
 */
static int run_external(char **argv,
                        int in_fd,
                        int out_fd,
                        int err_fd,
                        int background,
                        pid_t *pid_out,
                        const char *label,
                        int close_fd)
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

    /* A descriptor that `>&-` closes is closed in the child by a close action,
     * so the child's own opens take the lowest free numbers as in any shell.
     */
    for (int k = 0; k < 3; k++) {
        int fd = k == 0 ? in_fd : k == 1 ? out_fd : err_fd;

        if (fd == SH_FD_CLOSED &&
            (err = posix_spawn_file_actions_addclose(&actions, k)) != EOK)
            goto fail;
    }
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
    if (close_fd >= 0) {
        err = posix_spawn_file_actions_addclose(&actions, close_fd);
        if (err != EOK)
            goto fail;
    }
    if (!label)
        label = argv[0];

    if (sh_jc) {
        /* The first process of a pipeline starts its group; the rest join it.
         */
        if (posix_spawnattr_init(&attr) == EOK) {
            attr_p = &attr;
            (void) posix_spawnattr_setpgroup(attr_p, sh_jc_pgid);
            (void) posix_spawnattr_setflags(attr_p, POSIX_SPAWN_SETPGROUP);
        }
        snprintf(sh_jc_cmd, sizeof sh_jc_cmd, "%s", label);
    }

    /* Output the shell buffered so far must not land after the child's. */
    fflush(stdout);
    fflush(stderr);
    err = posix_spawnp(&pid, argv[0], actions_p, attr_p, argv, sh_environ());
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

            /* It may have read the terminal before it was ours and been stopped
             * by SIGTTIN; it is foreground now, so let it go on.
             */
            (void) kill(-pid, SIGCONT);
        }
    }
    if (background == 2)
        return 0;
    if (background) {
        struct sh_job *j =
            sh_job_add(sh_jc ? sh_jc_pgid : pid, pid, label, false);

        sh_last_bg = pid;
        if (j)
            fprintf(stderr, "[%d] %d\n", sh_job_number(j), pid);
        else
            fprintf(stderr, "%d\n", pid);
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

static int sh_redir_apply(int in_fd, int out_fd, int err_fd, int saved[3]);
static void sh_redir_restore(int saved[3]);

/* Keep fd out of the program an exec starts: it is a source or a saved copy of
 * a standard descriptor, not one the program is meant to inherit.
 */
static void exec_hide_fd(int fd)
{
    if (fd > STDERR_FILENO)
        (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
}

/* `exec` with no command makes its redirections the shell's own. With one, they
 * are the command's: applied for the exec and, if it fails, taken back so the
 * shell goes on with the descriptors it had.
 */
static int builtin_exec(char **argv, int in_fd, int out_fd, int err_fd)
{
    int saved[3];
    int err;

    if (!argv[1]) {
        if (in_fd == SH_FD_CLOSED)
            close(STDIN_FILENO);
        if (out_fd == SH_FD_CLOSED)
            close(STDOUT_FILENO);
        if (err_fd == SH_FD_CLOSED)
            close(STDERR_FILENO);
        if (in_fd >= 0 && in_fd != STDIN_FILENO)
            dup2(in_fd, STDIN_FILENO);
        if (out_fd >= 0 && out_fd != STDOUT_FILENO)
            dup2(out_fd, STDOUT_FILENO);
        if (err_fd >= 0 && err_fd != STDERR_FILENO)
            dup2(err_fd, STDERR_FILENO);
        return 0;
    }
    if (sh_redir_apply(in_fd, out_fd, err_fd, saved) != 0) {
        fprintf(stderr, "sh: redirection failed: %s\n", strerror(errno));
        sh_redir_restore(saved);
        return 1;
    }
    exec_hide_fd(in_fd);
    exec_hide_fd(out_fd);
    exec_hide_fd(err_fd);
    for (int k = 0; k < 3; k++)
        exec_hide_fd(saved[k]);
    execvp(argv[1], &argv[1]);
    err = errno;
    sh_redir_restore(saved);
    fprintf(stderr, "%s: %s\n", argv[1], strerror(err));
    return 127;
}

/* Subshell state.
 *
 * A builtin, a function, or a compound command that must not run in the shell
 * itself (it is in the background, or writes into a pipe whose reader is not
 * running yet) runs in a child `sh -S FD`. The parent writes the state the
 * child needs to FD, an inherited pipe: a run of NUL-terminated fields that
 * sh_state_run() reads back in the same order. Values are never quoted, so a
 * value cannot be mistaken for a field boundary or for shell syntax.
 *
 * Variables are not in it: every shell variable lives in environ (setenv),
 * which the child inherits. The traps are: `trap` lists them in the child, but
 * it never runs them (sh_state_run).
 */
#define SH_STATE_MAGIC "sh-state-1"

/* The shell a subshell runs: never looked up in PATH, which the script may have
 * pointed elsewhere.
 */
#ifndef SH_SELF_PATH
#define SH_SELF_PATH "/bin/sh"
#endif

/* Blob being built; err sticks once an allocation fails. */
struct sh_wblob {
    char *b;
    size_t len;
    size_t cap;
    bool err;
};

static void blob_put(struct sh_wblob *w, const char *s)
{
    if (!w->err && (append_str(&w->b, &w->len, &w->cap, s) != 0 ||
                    append_char(&w->b, &w->len, &w->cap, '\0') != 0))
        w->err = true;
}

static void blob_putn(struct sh_wblob *w, long v)
{
    char nb[24];

    snprintf(nb, sizeof nb, "%ld", v);
    blob_put(w, nb);
}

static void blob_put_lines(struct sh_wblob *w, char **lines, int n)
{
    blob_putn(w, n);
    for (int i = 0; i < n; i++)
        blob_put(w, lines[i]);
}

/* Serialise sh's state plus the command to run: kind 'a' is a simple command
 * that sh_print_argv printed, for run_line; 'c' is command text for run_text;
 * 's' is script lines for run_script_range.
 *
 * Returns the malloc'd blob (its size in *lenp), or NULL on allocation failure.
 */
static char *sh_state_blob(struct sh *sh,
                           char kind,
                           char **lines,
                           int nlines,
                           size_t *lenp)
{
    struct sh_wblob w = {0};
    char cwd[PATH_MAX];
    char opts[4];
    int nopt = 0;
    int njobs = 0;
    int ntraps = 0;
    char kb[2] = {kind, '\0'};
    mode_t mask = umask(0);

    (void) umask(mask);

    /* Where `set -e` is ignored (a condition), it stays ignored in the
     * subshell.
     */
    if (sh->opt_errexit && !sh->errexit_suppress)
        opts[nopt++] = 'e';
    if (sh->opt_nounset)
        opts[nopt++] = 'u';
    if (sh->opt_xtrace)
        opts[nopt++] = 'x';
    opts[nopt] = '\0';
    if (!getcwd(cwd, sizeof cwd))
        cwd[0] = '\0';
    for (int i = 0; i < SH_JOBS; i++)
        njobs += sh_jobs[i].pid != 0;
    for (int i = 0; i < SH_NTRAPS; i++)
        ntraps += sh->traps[i] != NULL;
    blob_put(&w, SH_STATE_MAGIC);
    blob_putn(&w, sh->last_status);
    blob_put(&w, opts);
    blob_putn(&w, (long) mask);
    blob_put(&w, cwd);
    blob_putn(&w, sh_pid());
    blob_putn(&w, sh_last_bg);
    blob_put(&w, sh->arg0 ? sh->arg0 : "sh");
    blob_put_lines(&w, sh->params, sh->nparams);
    blob_putn(&w, sh->nfuncs);
    for (int i = 0; i < sh->nfuncs; i++) {
        blob_put(&w, sh->funcs[i].name);
        blob_put_lines(&w, sh->funcs[i].body.line, sh->funcs[i].body.count);
    }
    blob_putn(&w, ntraps);
    for (int i = 0; i < SH_NTRAPS; i++) {
        if (sh->traps[i]) {
            blob_putn(&w, i);
            blob_put(&w, sh->traps[i]);
        }
    }
    blob_putn(&w, njobs);
    for (int i = 0; i < SH_JOBS; i++) {
        struct sh_job *j = &sh_jobs[i];

        if (!j->pid)
            continue;
        blob_putn(&w, i);
        blob_putn(&w, j->pgid);
        blob_putn(&w, j->pid);
        blob_putn(&w, j->stopped);
        blob_put(&w, j->cmd);
    }
    blob_put(&w, kb);
    blob_put_lines(&w, lines, nlines);
    if (w.err) {
        free(w.b);
        return NULL;
    }
    *lenp = w.len;
    return w.b;
}

/* Print argv back as one command line that, tokenized again, yields the same
 * words: every character but a few inert ones is backslash-escaped, a blank is
 * double-quoted, and an empty word is ''. The child thus neither expands,
 * splits nor globs a word a second time.
 *
 * Returns a malloc'd string, or NULL on allocation failure.
 */
static char *sh_print_argv(char **argv, int argc)
{
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];

        if (i > 0 && append_char(&buf, &len, &cap, ' ') != 0)
            goto oom;
        if (*s == '\0' && append_str(&buf, &len, &cap, "''") != 0)
            goto oom;
        for (; *s != '\0'; s++) {
            char q[4] = {'\\', *s, '\0', '\0'};

            if (isalnum((unsigned char) *s) || strchr("_-./=,:+@%", *s)) {
                q[0] = *s;
                q[1] = '\0';
            } else if (isspace((unsigned char) *s)) {
                /* Double-quoted: a line trim would eat an escaped blank. */
                q[0] = q[2] = '"';
            }
            if (append_str(&buf, &len, &cap, q) != 0)
                goto oom;
        }
    }
    return buf ? buf : strdup("");

oom:
    free(buf);
    return NULL;
}

/* Run lines (see sh_state_blob for kind) in a child shell holding sh's state,
 * with the given stdin/stdout/stderr; label names the job. Background and
 * pid_out behave as in run_external, which does the spawning, so the child
 * joins the job's process group; one of them must be set, since the state is
 * written only once the child is running.
 *
 * Returns 0, or nonzero when the child could not be started or given its state
 * (in the latter case it is still running, and *pid_out is set).
 */
static int sh_spawn_sub(struct sh *sh,
                        char kind,
                        char **lines,
                        int nlines,
                        int in_fd,
                        int out_fd,
                        int err_fd,
                        int background,
                        pid_t *pid_out,
                        const char *label)
{
    char fdarg[16];
    char *argv[] = {SH_SELF_PATH, "-S", fdarg, NULL};
    size_t len;
    size_t off = 0;
    char *blob = sh_state_blob(sh, kind, lines, nlines, &len);
    int pfd[2];
    int rc;

    if (!blob) {
        fprintf(stderr, "sh: out of memory\n");
        return 1;
    }
    if (pipe(pfd) != 0) {
        fprintf(stderr, "sh: pipe: %s\n", strerror(errno));
        free(blob);
        return 1;
    }

    /* Keep the state fd clear of the stdio fds the child's are dup2'ed onto. */
    if (pfd[0] <= STDERR_FILENO) {
        int fd = fcntl(pfd[0], F_DUPFD, STDERR_FILENO + 1);

        close(pfd[0]);
        pfd[0] = fd;
    }
    if (pfd[0] < 0) {
        fprintf(stderr, "sh: fcntl: %s\n", strerror(errno));
        close(pfd[1]);
        free(blob);
        return 1;
    }
    snprintf(fdarg, sizeof fdarg, "%d", pfd[0]);
    rc = run_external(argv, in_fd, out_fd, err_fd, background, pid_out, label,
                      pfd[1]);
    close(pfd[0]);
    if (rc == 0) {
        /* A child that died before reading its state must not take the shell
         * down with SIGPIPE.
         */
        struct sigaction ign = {.sa_handler = SIG_IGN};
        struct sigaction old;

        sigemptyset(&ign.sa_mask);
        sigaction(SIGPIPE, &ign, &old);
        while (off < len) {
            ssize_t w = write(pfd[1], blob + off, len - off);

            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                break;
            off += (size_t) w;
        }
        sigaction(SIGPIPE, &old, NULL);
        if (off < len) {
            fprintf(stderr, "sh: subshell state: %s\n", strerror(errno));
            rc = 1;
        }
    }
    close(pfd[1]);
    free(blob);
    return rc;
}

/* Redirect stdio to the in/out/err fds around a builtin call so commands like
 * `echo hi >file` and `read x | builtin` honour redirection.
 *
 * Returns 0 on success. The caller restores via sh_redir_restore().
 */
static int sh_redir_apply(int in_fd, int out_fd, int err_fd, int saved[3])
{
    const int want[3] = {in_fd, out_fd, err_fd};

    saved[0] = saved[1] = saved[2] = -1;

    /* Every descriptor is saved before any is closed, so that a copy never
     * takes the number of a slot that is about to be closed.
     */
    for (int k = 0; k < 3; k++) {
        if (want[k] != SH_FD_CLOSED && (want[k] < 0 || want[k] == k))
            continue;
        if (k == STDOUT_FILENO)
            fflush(stdout);
        else if (k == STDERR_FILENO)
            fflush(stderr);
        saved[k] = dup(k);
        if (saved[k] < 0 && !(want[k] == SH_FD_CLOSED && errno == EBADF))
            return -1; /* closing what is already closed is no error */
    }
    for (int k = 0; k < 3; k++) {
        if (want[k] == SH_FD_CLOSED ? (close(k) < 0 && errno != EBADF)
                                    : saved[k] >= 0 && dup2(want[k], k) < 0)
            return -1;
    }
    return 0;
}

static void sh_redir_restore(int saved[3])
{
    if (saved[1] >= 0) {
        fflush(stdout);
        clearerr(stdout); /* a write to a closed stdout failed */
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

/* Whether name is a special builtin (POSIX 2.15) this shell has. */
static bool is_special_builtin(const char *name)
{
    static const char *const special[] = {
        ":",      ".",   "break", "continue", "eval",  "exec", "exit",
        "export", "set", "shift", "return",   "unset", "trap", NULL,
    };

    for (int i = 0; special[i]; i++)
        if (!strcmp(special[i], name))
            return true;
    return false;
}

/* Set the variable a NAME=value word names. With saved, first store in *saved
 * what it was, as "NAME=old", or "NAME" when unset, for sh_unassign.
 *
 * Returns 0, or -1 with nothing changed.
 */
static int sh_assign(const char *word, char **saved)
{
    size_t n = (size_t) (strchr(word, '=') - word);
    char *name = dup_range(word, n);
    int rc;

    if (!name)
        return -1;
    if (saved) {
        const char *old = getenv(name);
        size_t len = n + (old ? strlen(old) + 1 : 0) + 1;

        *saved = malloc(len);
        if (!*saved) {
            free(name);
            return -1;
        }
        snprintf(*saved, len, "%s%s%s", name, old ? "=" : "", old ? old : "");
    }
    rc = setenv(name, word + n + 1, 1);
    free(name);
    if (rc != 0 && saved) {
        free(*saved);
        *saved = NULL;
    }
    return rc == 0 ? 0 : -1;
}

/* Undo sh_assign from what it saved, and free that. */
static void sh_unassign(char *saved)
{
    char *eq = strchr(saved, '=');

    if (eq) {
        *eq = '\0';
        setenv(saved, eq + 1, 1);
    } else {
        sh_unsetenv(saved);
    }
    free(saved);
}

/* Open the redirections in tok[a,b), left to right, into own[]: the new stdin,
 * stdout and stderr, or -1 where there is none. A `>&N` duplicates what fd N is
 * at that point, so `>f 2>&1` sends both to f: own[N] once it is redirected,
 * else pass[N], the fd the command would get, else N itself. The other words
 * are stored in argv (which may be NULL).
 *
 * Returns their count, or -1 after reporting a failed redirection, with own[]
 * closed.
 */
static int sh_open_redirs(struct sh_token *tok,
                          int a,
                          int b,
                          const int pass[3],
                          int own[3],
                          char **argv)
{
    int argc = 0;

    own[0] = own[1] = own[2] = -1;
    for (int i = a; i < b; i++) {
        enum sh_op op = tok[i].op;
        const char *path;
        int slot;
        int fd;

        if (op == SH_WORD) {
            if (argv)
                argv[argc] = tok[i].s;
            argc++;
            continue;
        }
        if (!is_redir_op(op) || i + 1 >= b || tok[i + 1].op != SH_WORD)
            continue;
        path = tok[++i].s;
        slot = op == SH_LT                                             ? 0
               : op == SH_ERRGT || op == SH_ERRGTGT || op == SH_DUPERR ? 2
                                                                       : 1;
        if ((op == SH_DUPOUT || op == SH_DUPERR) && !strcmp(path, "-")) {
            fd = SH_FD_CLOSED; /* closed in the command, not in the shell */
        } else if (op == SH_DUPOUT || op == SH_DUPERR) {
            char *end;
            long t = strtol(path, &end, 10);

            if (end == path || *end != '\0' || t < 0 || t > INT_MAX ||
                (t <= 2 && (own[t] == SH_FD_CLOSED ||
                            (own[t] == -1 && pass[t] == SH_FD_CLOSED)))) {
                fd = -1;
                errno = EBADF;
            } else {
                if (t <= 2)
                    t = own[t] >= 0 ? own[t] : pass[t] >= 0 ? pass[t] : t;
                fd = dup((int) t);
            }
        } else {
            int flags = slot == 0
                            ? O_RDONLY
                            : O_WRONLY | O_CREAT |
                                  (op == SH_GTGT || op == SH_ERRGTGT ? O_APPEND
                                                                     : O_TRUNC);

            /* ipc-send: utils ->
             * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
             */
            fd = open(path, flags, 0666);
        }
        if (fd < 0 && fd != SH_FD_CLOSED) {
            fprintf(stderr, "sh: %s: %s\n", path, strerror(errno));
            for (int k = 0; k < 3; k++) {
                close_if_needed(own[k]);
                own[k] = -1;
            }
            return -1;
        }
        close_if_needed(own[slot]); /* `cmd <a <b` keeps the last */
        own[slot] = fd;
    }
    return argc;
}

/* Run the simple command tok[a,b). Set `sub` when it is in the background, or
 * feeds a pipe whose reader the shell has yet to start: what would run inside
 * the shell then runs in a child shell (sh_spawn_sub).
 */
static int command_from_tokens(struct sh *sh,
                               struct sh_token *tok,
                               int a,
                               int b,
                               int in_fd,
                               int out_fd,
                               int err_fd,
                               int background,
                               pid_t *pid_out,
                               int sub)
{
    char *argv[SH_MAX_TOKENS];
    int argc;
    const int pass[3] = {in_fd, out_fd, err_fd};
    int own[3];
    int handled = 0;
    int skip_func = 0;
    int nassign = 0;            /* leading NAME=value words */
    bool keep = true;           /* whether they outlive the command */
    char *saved[SH_MAX_TOKENS]; /* what they replaced, for sh_unassign */
    int rc;

    argc = sh_open_redirs(tok, a, b, pass, own, argv);
    if (argc < 0)
        return 1;
    argv[argc] = NULL;

    /* A pending here-document feeds stdin of the pipeline's first stage (the
     * one with no upstream pipe, in_fd < 0), unless an explicit `<` overrides
     * it.
     */
    if (own[0] < 0 && in_fd < 0 && sh->heredoc_fd >= 0)
        in_fd = sh->heredoc_fd;
    in_fd = own[0] != -1 ? own[0] : in_fd;
    out_fd = own[1] != -1 ? own[1] : out_fd;
    err_fd = own[2] != -1 ? own[2] : err_fd;

    /* What would run inside the shell -- a lone assignment, a builtin, a
     * function -- runs in a child shell instead when `sub` is set. The child
     * gets the words, already expanded, and the redirected fds.
     */
    while (nassign < argc && is_assignment(argv[nassign]))
        nassign++;
    if (sub && argc > 0) {
        char **cv = argv + nassign;
        int cc = argc - nassign;
        int k = cc >= 2 && !strcmp(cv[0], "command") && strcmp(cv[1], "-v") &&
                strcmp(cv[1], "-V");

        if (cc == 0 || is_builtin(cv[k]) || (!k && sh_find_func(sh, cv[0]))) {
            char *line = sh_print_argv(argv, argc);

            if (!line)
                fprintf(stderr, "sh: out of memory\n");
            rc = line ? sh_spawn_sub(sh, 'a', &line, 1, in_fd, out_fd, err_fd,
                                     background, pid_out, argv[0])
                      : 1;
            free(line);
            goto done;
        }
    }

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

    /* NAME=value words: alone, they set shell variables. Before a command, they
     * are in its environment only, except for a special builtin, where they
     * stay (POSIX 2.9.1.2).
     */
    keep = nassign == argc || is_special_builtin(argv[nassign]);
    for (int i = 0; i < nassign; i++) {
        if (sh_assign(argv[i], keep ? NULL : &saved[i]) != 0) {
            nassign = i;
            rc = 1;
            goto done;
        }
    }
    if (nassign == argc) { /* the status of the last substitution, else 0 */
        rc = sh->subst_status > 0 ? sh->subst_status : 0;
        goto done;
    }
    memmove(argv, argv + nassign,
            (size_t) (argc - nassign + 1) * sizeof(*argv));
    argc -= nassign;

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
        clearerr(stdout); /* an error of an earlier write is not this one's */
        rc = run_builtin(sh, argv, argc, in_fd, &handled);
        if (handled && (fflush(stdout) != 0 || ferror(stdout))) {
            fprintf(stderr, "sh: %s: write error: %s\n", argv[0],
                    strerror(errno));
            clearerr(stdout);
            if (rc == 0)
                rc = 1;
        }
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
        rc = run_external(argv, in_fd, out_fd, err_fd, background, pid_out,
                          NULL, -1);
done:
    for (int i = nassign - 1; !keep && i >= 0; i--) /* X=1 X=2: last first */
        sh_unassign(saved[i]);
    for (int k = 0; k < 3; k++)
        close_if_needed(own[k]);
    return rc;
}

/* Without job control, an asynchronous list reads /dev/null rather than the
 * shell's input (POSIX), unless a here-document or a redirection says
 * otherwise.
 *
 * Returns the fd to use as its stdin, or -1 for the shell's own.
 */
static int sh_bg_stdin(struct sh *sh)
{
    if (sh_jc || sh->heredoc_fd >= 0)
        return -1;

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    return open("/dev/null", O_RDONLY | O_CLOEXEC);
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

                /* The writer must not hold its own pipe's read end. A read end
                 * on a stdio fd (the shell's was closed) is not dup2'ed by the
                 * reader's spawn, so it must stay inheritable.
                 */
                if (pfd[0] > STDERR_FILENO)
                    (void) fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
                out = pfd[1];
            }
            if (background) {
                /* One job per pipeline: its last stage's. */
                if (input < 0)
                    input = sh_bg_stdin(sh);
                rc = command_from_tokens(sh, tok, start, i, input, out, -1,
                                         is_last ? 1 : 2, NULL, 1);
            } else if (!is_last) {
                /* Foreground intermediate: spawn without waiting; the shell
                 * waits after the last stage exits.
                 */
                rc = command_from_tokens(sh, tok, start, i, input, out, -1, 0,
                                         &pid, 1);
                if (pid > 0 && npids < (int) (sizeof(pids) / sizeof(pids[0])))
                    pids[npids++] = pid;
            } else {
                /* Foreground last: spawn, then wait below for the exit status.
                 * Builtins set pid=-1 and return the status in rc.
                 */
                rc = command_from_tokens(sh, tok, start, i, input, out, -1, 0,
                                         &pid, 0);
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

    /* A stopped job's other stages are stopped with it; do not wait for them.
     */
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

/* The words that open a compound command. */
static const char *const sh_compound[] = {"if",   "for", "while", "until",
                                          "case", "{",   NULL};

/* Whether s starts with one of the words in kw (NULL-terminated). */
static bool starts_any(const char *s, const char *const *kw)
{
    for (; *kw; kw++)
        if (starts_word(s, *kw))
            return true;
    return false;
}

/* Whether the text from start up to at, which points at a word, holds nothing
 * before it that makes it an argument: only blanks and the words after which a
 * command comes.
 */
static bool at_command_start(const char *start, const char *at)
{
    static const char *const lead[] = {"{", "then", "do", "else", "!", NULL};

    for (;;) {
        start += strspn(start, " \t");
        if (start == at)
            return true;
        if (!starts_any(start, lead))
            return false;
        while (!strchr(" \t", *start))
            start++;
    }
}

/* Whether the case word at points at, in the command text from start, begins a
 * command rather than being an argument or a pattern.
 */
static bool case_at_command(const char *start, const char *at)
{
    const char *b = at;

    static const char *const lead[] = {"then", "do", "else", "!", NULL};

    while (b > start && (b[-1] == ' ' || b[-1] == '\t'))
        b--;
    if (b == start || strchr(";&|(){\n", b[-1]) != NULL)
        return true;
    while (b > start && !strchr(" \t\n;&|(){", b[-1]))
        b--; /* the word before it */
    return starts_any(b, lead);
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
    struct sh_scan sc = {line, 0};
    struct sh_unit u;
    const char *seg = line;
    const char *end = NULL; /* where a comment cuts the line off */
    int n = 0;

    for (;;) {
        const char *p = sc.p;
        char c = *p;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            break;
        if (u.kind != SH_U_LIT || sc.quote != 0)
            continue; /* quoted, escaped or a substitution: never a separator */

        /* A `#` starts a comment only at a word boundary, so it does not
         * mistake the `#` in a `${VAR#pat}` expansion for one; a `>&N`
         * redirection's `&` is not a separator.
         */
        if (c == '#' && is_comment_start(line, p)) {
            end = p;
            break;
        }
        if (c == '>' && p[1] == '&') {
            sc.p = p + 2;
            while (isdigit((unsigned char) *sc.p))
                sc.p++;
            continue;
        }

        /* A case command is one statement, with the ;; and ; of its arms. */
        if (c == 'c' && starts_word(p, "case") && at_command_start(seg, p)) {
            const char *esac = sh_scan_case_end(p);

            if (esac) {
                sc.p = esac;
                continue;
            }
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
            sc.p = p + adv;
            seg = sc.p;
        }
    }
    if (!end)
        end = sc.p;
    if (n >= max || !(out[n].text = dup_range(seg, (size_t) (end - seg))))
        goto fail;
    out[n++].sep = SH_SEMI;
    return n;

fail:
    while (n > 0)
        free(out[--n].text);
    return -1;
}

/* Run the list on line as if it continued one whose last command ended in
 * `prev` with status rc: after SH_AND or SH_OR, its first AND-OR operands may
 * be skipped.
 */
static int run_list(struct sh *sh, const char *line, enum sh_op prev, int rc)
{
    struct sh_stmt *stmt;
    int nstmt;

    if (!line_is_complete(sh, line))
        return 2;
    stmt = malloc(sizeof(*stmt) * SH_MAX_TOKENS);
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
            sh->subst_status = -1;
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
        sh_run_traps(sh);
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

static int run_line(struct sh *sh, const char *line)
{
    return run_list(sh, line, SH_SEMI, 0);
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

/* Scan s for the braces that open and close a function body, adding to *depth
 * for each unquoted { and subtracting for each }; a brace that is quoted,
 * escaped or part of a ${...} or a substitution does not count.
 *
 * Returns, if stop, the } that brings *depth to 0, else NULL.
 */
static const char *brace_scan(const char *s, int *depth, bool stop)
{
    struct sh_scan sc = {s, 0};
    struct sh_unit u;

    for (;;) {
        const char *at = sc.p;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            return NULL;
        if (u.kind != SH_U_LIT || sc.quote != 0)
            continue;
        if (*at == '{')
            ++*depth;
        else if (*at == '}' && --*depth == 0 && stop)
            return at;
    }
}

/* The net brace depth a line contributes. */
static int brace_delta(const char *s)
{
    int d = 0;

    (void) brace_scan(s, &d, false);
    return d;
}

/* If sc->line[*ip] begins a function definition `name() ...`, capture its body
 * into the function table and advance *ip past the closing `}`.
 *
 * Returns 1 if a definition was consumed, 0 if the line is not one, -1 on
 * error. Handles the body brace on the def line, on the next line, and a
 * one-line `name() { ...; }`, after which the rest of the line (`; f &`), if
 * any, is added to *rest for the caller to run.
 */
static int heredoc_skip(struct sh_script *sc, int i, int to, char *line);

static int capture_func_def(struct sh *sh,
                            struct sh_script *sc,
                            int *ip,
                            int to,
                            struct sh_script *rest)
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
            int depth = 1;

            /* The body ends at the `}` that balances the opening one. */
            snprintf(body, sizeof(body), "%s", p);
            rb = (char *) brace_scan(body, &depth, true);
            if (!rb)
                rb = body + strlen(body);
            if (*rb != '\0') {
                *rb++ = '\0';
                rb += strspn(rb, " \t");
                rb += *rb == ';';
                if (*rb != '\0' &&
                    (script_add(rest, rb) != 0 || script_explode(rest) != 0))
                    return -1;
            }
            trim(body);
            if (script_add(&one, body) != 0 || script_explode(&one) != 0 ||
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
           (line[n] == '\0' || strchr(" \t;\n|&<>", line[n]));
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
    struct sh_scan sc = {line, 0};
    struct sh_unit u;

    for (;;) {
        char *p = (char *) sc.p;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            return NULL;
        if (u.kind != SH_U_LIT || sc.quote != 0)
            continue;
        if (*p == '#' && is_comment_start(line, p))
            return NULL; /* a comment */
        if (*p == '<' && p[1] == '<') {
            if (p[2] != '<')
                return p;
            sc.p = p + 3; /* a here-string */
        }
    }
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

/* Whether line goes on on the next one: it ends in an unquoted &&, || or |
 * (before any comment), or in a backslash. *cut gets the length to keep, and
 * *blank whether to join the next line with a blank.
 */
static bool line_continues(const char *line, size_t *cut, bool *blank)
{
    struct sh_scan sc = {line, 0};
    struct sh_unit u;
    bool op = false;  /* the last character is an operator that continues */
    bool amp = false; /* it is an unquoted & that does not end a && */
    bool esc = false; /* it is a backslash at the very end */
    size_t end = 0;   /* just past it */

    for (;;) {
        const char *p = sc.p;
        bool plain; /* an unquoted character with no meaning of its own */

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            break;
        plain = u.kind == SH_U_LIT && sc.quote == 0;
        if (plain && (*p == ' ' || *p == '\t'))
            continue;
        if (plain && *p == '#' && is_comment_start(line, p))
            break;
        if (plain && *p != '\\') {
            op = *p == '|' || (*p == '&' && amp);
            amp = *p == '&' && !amp;
        } else {
            esc = plain; /* only a lone backslash is a plain \ */
            op = amp = false;
        }
        end = (size_t) (sc.p - line);
    }
    if (sc.quote)
        return false;
    *cut = esc ? end - 1 : end;
    *blank = !esc;
    return esc || op;
}

/* Join each line of sc that line_continues with the next, at load time, leaving
 * here-document bodies alone.
 *
 * Returns 0, or -1 on allocation failure.
 */
static int script_join_lines(struct sh_script *sc)
{
    for (int i = 0; i < sc->count; i++) {
        char t[SH_LINE_MAX];
        char sep = ' ';
        size_t cut;
        size_t n;
        bool blank;
        char *j;

        snprintf(t, sizeof t, "%s", sc->line[i]);
        trim(t);
        if (heredoc_find(t)) {
            i = heredoc_skip(sc, i, sc->count, t);
            continue;
        }
        if (i + 1 >= sc->count)
            break;
        if (!line_continues(sc->line[i], &cut, &blank)) {
            /* A quote or a substitution still open at the end of the line goes
             * on in the next one, with the newline in it.
             */
            if (!line_open_join(sc->line[i], &cut, &blank))
                continue;
            sep = '\n';
        }
        n = strlen(sc->line[i + 1]);
        j = realloc(sc->line[i], cut + blank + n + 1);
        if (!j)
            return -1;
        j[cut] = sep;
        memcpy(j + cut + blank, sc->line[i + 1], n + 1);
        sc->line[i] = j;
        free(sc->line[i + 1]);
        memmove(&sc->line[i + 1], &sc->line[i + 2],
                (size_t) (sc->count - i - 2) * sizeof(*sc->line));
        sc->count--;
        i--; /* the joined line may go on again */
    }
    return 0;
}

static bool starts_func_def(const char *s);

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
        if (starts_word(tmp, open1) ||
            (!strcmp(open1, "{") && starts_func_def(tmp) && tmp[0] != '\0' &&
             tmp[strlen(tmp) - 1] == '{')) /* a function opens a group too */
            depth++;
        if (depth == 0 && mid && *mid_out < 0 &&
            (starts_word(tmp, mid) ||
             (!strcmp(mid, "else") && starts_word(tmp, "elif"))))
            *mid_out = i; /* the first else, or elif, of an if */
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

/* The runners below take the opening line `at` and the closing line, which
 * compound_end found; run_block leaves the caller past the latter.
 *
 * An if's else_i is its first else or elif line: an elif is run as an if of its
 * own that ends at the same fi.
 */
static int run_if(struct sh *sh,
                  struct sh_script *sc,
                  int at,
                  int fi_i,
                  int else_i)
{
    char cond[SH_LINE_MAX];
    char *thenp;
    int then_i;
    int kw = starts_word(sc->line[at], "elif") ? 4 : 2;
    int rc;

    snprintf(cond, sizeof(cond), "%s", sc->line[at]);
    trim(cond);
    memmove(cond, cond + kw, strlen(cond + kw) + 1);
    trim(cond);
    thenp = strstr(cond, "; then");
    if (thenp) {
        *thenp = '\0';
        then_i = at + 1;
    } else {
        size_t clen = strlen(cond);

        if (clen >= 4 && !strcmp(cond + clen - 4, "then")) {
            cond[clen - 4] = '\0';
            then_i = at + 1;
        } else {
            then_i = at + 2;
        }
    }
    trim(cond);
    sh->errexit_suppress++; /* set -e is ignored while testing the condition */
    rc = run_line(sh, cond);
    sh->errexit_suppress--;
    if (rc == 0) {
        rc = run_script_range(sh, sc, then_i, else_i >= 0 ? else_i : fi_i);
    } else if (else_i >= 0 && starts_word(sc->line[else_i], "elif")) {
        int next;

        (void) find_matching(sc, else_i + 1, fi_i, "if", "else", "fi", &next);
        rc = run_if(sh, sc, else_i, fi_i, next);
    } else if (else_i >= 0) {
        rc = run_script_range(sh, sc, else_i + 1, fi_i);
    } else {
        rc = 0; /* no condition held and there is no else: success */
    }
    return rc;
}

static int run_for(struct sh *sh, struct sh_script *sc, int at, int done_i)
{
    struct sh_token tok[SH_MAX_TOKENS];
    struct sh_words vals = {0};
    char *var = NULL;
    int ntok;
    bool in;
    int rc = 0;

    ntok = line_is_complete(sh, sc->line[at]) ? tokenize(sh, sc->line[at], tok)
                                              : -1;

    /* tok[1] (loop var) must be a word: an operator token carries s == NULL, so
     * strcmp/strdup on it would dereference NULL. Without `in`, the loop goes
     * over "$@".
     */
    if (ntok < 2 || tok[1].op != SH_WORD || strcmp(tok[0].s, "for") != 0) {
        token_free(tok, ntok > 0 ? ntok : 0);
        return 2;
    }
    in = ntok >= 3 && tok[2].op == SH_WORD && !strcmp(tok[2].s, "in");
    var = strdup(tok[1].s);
    for (int i = 3; in && i < ntok && rc == 0; i++)
        if (tok[i].op == SH_WORD && strcmp(tok[i].s, "do") != 0)
            rc = words_add(&vals, tok[i].s);
    for (int i = 0; !in && i < sh->nparams && rc == 0; i++)
        rc = words_add(&vals, sh->params[i]);
    token_free(tok, ntok);
    if (rc != 0 || !var) {
        free(var);
        words_free(&vals);
        return 1;
    }
    sh->loop_depth++;
    for (int i = 0; i < vals.n && !sh->exit_requested; i++) {
        if (setenv(var, vals.v[i], 1) != 0) {
            fprintf(stderr, "sh: %s: %s\n", var, strerror(errno));
            rc = 1;
            break;
        }
        rc = run_script_range(sh, sc, at + 1, done_i);
        if (loop_consume(sh))
            break;
    }
    sh->loop_depth--;
    free(var);
    words_free(&vals);
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
    struct sh_scan sc = {s, 0};
    struct sh_unit u;

    for (;;) {
        char *p = (char *) sc.p;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            return NULL;
        if (u.kind == SH_U_LIT && sc.quote == 0 && p[0] == 'd' && p[1] == 'o' &&
            (p == s || p[-1] == ' ' || p[-1] == '\t' || p[-1] == ';') &&
            (p[2] == '\0' || p[2] == ' ' || p[2] == '\t' || p[2] == ';'))
            return p;
    }
}

/* while/until COND; do BODY; done. `until` inverts the condition sense. */
static int run_while(struct sh *sh,
                     struct sh_script *sc,
                     int at,
                     int done_i,
                     int until)
{
    char cond[SH_LINE_MAX];
    char *dop;
    int do_i;
    int rc = 0;

    snprintf(cond, sizeof(cond), "%s", sc->line[at]);
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
        do_i = at + 1;
    } else {
        /* `do` must be alone on the next line, else the loop is malformed */
        char nl[SH_LINE_MAX];

        snprintf(nl, sizeof(nl), "%s", sc->line[at + 1]);
        trim(nl);
        if (strcmp(nl, "do") != 0)
            return 2;
        do_i = at + 2;
    }
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
    return rc;
}

/* Whether word matches one of the alternatives of a case pattern list, a|b|c,
 * which may open with the ( that POSIX allows. Each alternative is expanded as
 * a pattern, so a quoted character in it matches only itself.
 */
static bool case_pattern_matches(struct sh *sh,
                                 const char *list,
                                 const char *word)
{
    char *pats = strdup(list + (*list == '('));
    char *alt = pats;
    bool hit = false;

    while (pats && alt) {
        char *bar = (char *) sh_scan_find(alt, "|");
        char *pat;

        if (bar)
            *bar = '\0';
        trim(alt);
        pat = expand_word(sh, alt, true, 0);
        hit = fnmatch(pat ? pat : alt, word, 0) == 0;
        free(pat);
        if (hit || !bar)
            break;
        alt = bar + 1;
    }
    free(pats);
    return hit;
}

/* The ;; that ends the arm whose command text is rp, or NULL: the first one
 * outside quotes and outside a case command of the arm's own, or none while
 * such a case command is not yet closed.
 */
static char *arm_end(char *rp)
{
    struct sh_scan sc = {rp, 0};
    struct sh_unit u;

    for (;;) {
        const char *at = sc.p;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            return NULL;
        if (u.kind != SH_U_LIT || sc.quote != 0)
            continue;
        if (starts_word(at, "case") && case_at_command(rp, at)) {
            const char *esac = sh_scan_case_end(at);

            if (!esac)
                return NULL; /* its esac is on a later line: the arm goes on */
            sc.p = esac;
        } else if (*at == ';' && at[1] == ';') {
            return (char *) at;
        }
    }
}

static int run_case(struct sh *sh, struct sh_script *sc, int at, int esac_i)
{
    struct sh_token tok[SH_MAX_TOKENS];
    char *word = NULL;
    int ntok;
    int rc = 0;

    ntok = line_is_complete(sh, sc->line[at]) ? tokenize(sh, sc->line[at], tok)
                                              : -1;

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
    for (int i = at + 1; i < esac_i && !sh->exit_requested; i++) {
        char line[SH_LINE_MAX];
        char *rp;
        char *arm;

        snprintf(line, sizeof(line), "%s", sc->line[i]);
        trim(line);
        rp = (char *) sh_scan_find(line,
                                   ")"); /* the one that ends the pattern */
        if (!rp)
            continue;
        *rp++ = '\0';
        trim(line);

        /* The arm goes on over the following lines until its ;; (or the esac),
         * whether or not it is the one that runs.
         */
        arm = strdup(rp);
        while (arm && !arm_end(arm) && i + 1 < esac_i) {
            size_t n = strlen(arm);
            size_t m = strlen(sc->line[i + 1]);
            char *grown = realloc(arm, n + m + 2);

            if (!grown) {
                free(arm);
                arm = NULL;
                break;
            }
            arm = grown;
            arm[n] = '\n';
            memcpy(arm + n + 1, sc->line[++i], m + 1);
        }
        if (!arm) {
            rc = 1;
            break;
        }
        if (case_pattern_matches(sh, line, word)) {
            char *end = arm_end(arm);

            if (end)
                *end = '\0';
            trim(arm);
            rc = run_text(sh, arm); /* a list that may hold compound commands */
            free(arm);
            break;
        }
        free(arm);
    }
    free(word);
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
        struct sh_scan sc = {p, 0};
        struct sh_unit u;

        /* No quote removal: ' and " are text, and only $ ` and \ are special.
         */
        if (*p == '\\' && (p[1] == '$' || p[1] == '`' || p[1] == '\\')) {
            p++;
            if (append_char(&buf, &len, &cap, *p++) != 0)
                goto oom;
            continue;
        }
        if (*p != '$' && *p != '`') {
            if (append_char(&buf, &len, &cap, *p++) != 0)
                goto oom;
            continue;
        }
        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_ARITH) {
            char nb[32];

            p += u.len;
            if (unit_arith(sh, &u, false, nb) != 0 ||
                append_str(&buf, &len, &cap, nb) != 0)
                goto oom;
        } else if (u.kind == SH_U_PAREN || u.kind == SH_U_BQ) {
            char *result;

            p += u.len;
            if (unit_cmdsub(sh, &u, false, &result) != 0)
                goto oom;
            if (append_str(&buf, &len, &cap, result) != 0) {
                free(result);
                goto oom;
            }
            free(result);
        } else {
            p++;
            if (expand_dollar(sh, &p, &buf, &len, &cap, XF_DQ) != 0)
                goto oom;
        }
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

/* Split a line holding a whole compound command (`while :; do :; done &`) into
 * the one-keyword-per-line form the block runners expect: a statement per
 * line, `do`, `then` and `else` on lines of their own, `&&`/`||` lists and a
 * trailing `&` kept on their line.
 *
 * Returns 1 with the lines in *out, or 0 when the line does not open and close
 * a compound command (a `case`, or a block continued on later lines), which is
 * then run as before.
 */
static int sh_explode(const char *line, struct sh_script *out)
{
    static const char *const lead[] = {"do", "then", "else", NULL};
    static const char *const openers[] = {"if", "for", "while", "until", NULL};
    static const char *const close[] = {"fi", "done", NULL};
    struct sh_stmt *st;
    char cur[SH_LINE_MAX];
    size_t used = 0;
    int depth = 0;
    int opened = 0;
    int braces = 0; /* the line has a { or } word of a brace group */
    int cased = 0;  /* and a case command of its own */
    int ok = 1;
    int n;

    /* Cheap filter: a whole compound on one line has a `;` and ends one, and a
     * brace group has its braces.
     */
    if (!starts_word(line, "{") && !strchr(line, '}') &&
        !strstr(line, "esac") &&
        (!strchr(line, ';') || (!strstr(line, "done") && !strstr(line, "fi"))))
        return 0;
    st = malloc(sizeof(*st) * SH_MAX_TOKENS);
    if (!st)
        return 0;
    n = split_statements(line, st, SH_MAX_TOKENS);
    for (int i = 0; i < n && ok; i++) {
        char *t = st[i].text;
        int w;

        trim(t);
        if (used == 0) {
            for (int k = 0; lead[k]; k++) {
                if (starts_word(t, lead[k])) {
                    ok = script_add(out, lead[k]) == 0;
                    t += strlen(lead[k]);
                    t += strspn(t, " \t");
                    break;
                }
            }
            while (starts_word(t, "{")) {
                ok = script_add(out, "{") == 0;
                t += 1 + strspn(t + 1, " \t");
                braces = 1;
            }
            if (starts_word(t, "case")) {
                struct sh_script sub = {0};

                /* The command's own lines, if nothing goes on after it. */
                cased = 1;
                ok = st[i].sep == SH_SEMI && case_explode(t, &sub);
                for (int k = 0; k < sub.count && ok; k++)
                    ok = script_add(out, sub.line[k]) == 0;
                script_free(&sub);
                continue;
            }
            if (starts_any(t, openers)) {
                depth++;
                opened = 1;
            } else if (starts_any(t, close)) {
                /* A pipe or redirection stays on the `done`/`fi` line, for
                 * run_compound.
                 */
                if (--depth < 0)
                    ok = 0;
            } else if (starts_word(t, "}")) {
                braces = 1; /* its balance is the block scanner's to check */
            }
        }
        if (used != 0 && starts_word(t, "case"))
            ok = 0; /* a case command after && or || is not split */
        if (*t == '\0' && st[i].sep != SH_BG)
            continue;
        w = snprintf(cur + used, sizeof cur - used, "%s%s", t,
                     st[i].sep == SH_AND  ? " && "
                     : st[i].sep == SH_OR ? " || "
                     : st[i].sep == SH_BG ? " &"
                                          : "");
        if (w < 0 || (size_t) w >= sizeof cur - used) {
            ok = 0;
        } else if (st[i].sep == SH_AND || st[i].sep == SH_OR) {
            used += (size_t) w;
        } else {
            ok = script_add(out, cur) == 0;
            used = 0;
        }
    }
    for (int i = 0; i < n; i++)
        free(st[i].text);
    free(st);
    if (ok && used == 0 &&
        ((opened && depth == 0) || (!opened && (braces || cased))))
        return 1;
    script_free(out);
    return 0;
}

/* Split a case command that is all on one line (`case W in a) x;; b) y;; esac`)
 * into the lines run_case reads: `case W in`, a line for each arm, and the
 * `esac` with whatever follows it.
 *
 * Returns 1 with the lines in *out, or 0 when the line is not one.
 */
static int case_explode(const char *line, struct sh_script *out)
{
    struct sh_scan sc;
    struct sh_unit u;
    const char *p = line + strspn(line, " \t");
    const char *head;
    const char *arm;
    const char *esac = NULL;
    char buf[SH_LINE_MAX];
    int ok = 1;

    if (!starts_word(p, "case") || !strstr(p, "esac"))
        return 0;
    head = p;
    p += 4 + strspn(p + 4, " \t");

    /* The word, which ends at the first blank outside quotes and substitutions,
     * and the `in` after it.
     */
    sc = (struct sh_scan) {p, 0};
    for (;;) {
        const char *at = sc.p;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            return 0;
        if (u.kind == SH_U_LIT && sc.quote == 0 &&
            (*at == ' ' || *at == '\t')) {
            p = at;
            break;
        }
    }
    p += strspn(p, " \t");
    if (!starts_word(p, "in"))
        return 0;
    p += 2;
    snprintf(buf, sizeof buf, "%.*s", (int) (p - head), head);
    if (script_add(out, buf) != 0)
        return 0;

    /* The arms, which end at ;; ; the last one, at the esac that is a command.
     */
    arm = p;
    sc = (struct sh_scan) {p, 0};
    for (;;) {
        const char *at = sc.p;
        bool end_arm = false;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            break;
        if (u.kind != SH_U_LIT || sc.quote != 0)
            continue;
        if (starts_word(at, "case") && case_at_command(arm, at)) {
            const char *nested = sh_scan_case_end(at);

            /* A case command inside an arm has ;; and esac of its own: it is
             * all part of the arm.
             */
            if (!nested) {
                script_free(out);
                return 0;
            }
            sc.p = nested;
            continue;
        }
        if (*at == ';' && at[1] == ';') {
            sc.p = at + 2;
            end_arm = true;
        } else if (starts_word(at, "esac") &&
                   (at == arm || at[-1] == ' ' || at[-1] == '\t' ||
                    at[-1] == ';')) {
            const char *b = at;

            while (b > arm && (b[-1] == ' ' || b[-1] == '\t'))
                b--;
            if (b == arm || b[-1] == ';' || b[-1] == ')') {
                esac = at;
                end_arm = true;
            }
        }
        if (!end_arm)
            continue;
        {
            const char *stop = esac ? esac : at;
            size_t n = (size_t) (stop - arm);
            char *t = dup_range(arm, n);

            if (!t) {
                script_free(out);
                return 0;
            }
            trim(t);
            n = strlen(t);
            while (n > 0 && t[n - 1] == ';')
                t[--n] = '\0';
            trim(t);
            if (*t != '\0') {
                snprintf(buf, sizeof buf, "%s ;;", t);
                ok = script_add(out, buf) == 0;
            }
            free(t);
        }
        if (esac || !ok)
            break;
        arm = sc.p;
    }
    if (!esac || !ok) {
        script_free(out);
        return 0;
    }
    return script_add(out, esac) == 0 ? 1 : 0;
}

/* Whether s starts with NAME(), a function definition. */
static bool starts_func_def(const char *s)
{
    s += strspn(s, " \t");
    if (!isalpha((unsigned char) *s) && *s != '_')
        return false;
    while (is_name_char((unsigned char) *s))
        s++;
    s += strspn(s, " \t");
    if (*s++ != '(')
        return false;
    s += strspn(s, " \t");
    return *s == ')';
}

/* Split every line of sc that holds a whole compound command into the
 * one-keyword-per-line form the block runners expect (sh_explode), once, when
 * the script is loaded. Here-document bodies are left alone, and so is a
 * function definition, whose body is split when it is defined.
 *
 * Returns 0, or -1 on allocation failure.
 */
static int script_explode(struct sh_script *sc)
{
    struct sh_script out = {0};
    int rc = 0;

    for (int i = 0; i < sc->count && rc == 0; i++) {
        struct sh_script one = {0};
        char t[SH_LINE_MAX];
        int last = i;

        snprintf(t, sizeof t, "%s", sc->line[i]);
        trim(t);
        if (heredoc_find(t))
            last = heredoc_skip(sc, i, sc->count, t);
        else if (!starts_func_def(t) && (sh_explode(sc->line[i], &one) ||
                                         case_explode(sc->line[i], &one)))
            last = -1; /* sh_explode splits a line of several commands */
        for (int k = 0; k < one.count && rc == 0; k++)
            rc = script_add(&out, one.line[k]);
        for (; i <= last && rc == 0; i++)
            rc = script_add(&out, sc->line[i]);
        if (last >= 0)
            i--;
        script_free(&one);
    }
    if (rc != 0) {
        script_free(&out);
        return -1;
    }
    script_free(sc);
    *sc = out;
    return 0;
}

/* The line that closes the compound command opening at line i (its fi, done or
 * esac), or -1 if there is none; *mid gets the line of an if's else, or -1.
 * This is the one scan of the command's lines.
 */
static int compound_end(struct sh_script *sc, int i, int to, int *mid)
{
    bool is_if = starts_word(sc->line[i], "if");

    *mid = -1;
    if (starts_word(sc->line[i], "{"))
        return find_matching(sc, i + 1, to, "{", NULL, "}", mid);
    if (is_if || starts_word(sc->line[i], "case"))
        return find_matching(sc, i + 1, to, is_if ? "if" : "case",
                             is_if ? "else" : NULL, is_if ? "fi" : "esac", mid);
    return find_done(sc, i + 1, to);
}

/* Run the compound command from line *ip to its closing line end, with an if's
 * else at line mid, and leave *ip at end.
 */
static int run_block(struct sh *sh,
                     struct sh_script *sc,
                     int *ip,
                     int end,
                     int mid)
{
    const char *line = sc->line[*ip];
    int rc;

    if (starts_word(line, "if"))
        rc = run_if(sh, sc, *ip, end, mid);
    else if (starts_word(line, "for"))
        rc = run_for(sh, sc, *ip, end);
    else if (starts_word(line, "while") || starts_word(line, "until"))
        rc = run_while(sh, sc, *ip, end, starts_word(line, "until"));
    else if (starts_word(line, "{"))
        rc = run_script_range(sh, sc, *ip + 1, end);
    else
        rc = run_case(sh, sc, *ip, end);
    *ip = end;
    sh->last_status = rc; /* a loop or an if that ran nothing is a success */
    return rc;
}

/* Copy lines [i,end) of sc and then `last` into *sub, a script of its own.
 *
 * Returns 0, or -1 on allocation failure.
 */
static int sh_sub_script(struct sh_script *sc,
                         int i,
                         int end,
                         const char *last,
                         struct sh_script *sub)
{
    for (int k = i; k < end; k++)
        if (script_add(sub, sc->line[k]) != 0)
            return -1;
    return script_add(sub, last);
}

/* The length of the keyword that closes a compound command: fi, done, esac or a
 * brace group's }.
 */
static int closer_len(const char *line)
{
    return starts_word(line, "fi") ? 2 : starts_word(line, "}") ? 1 : 4;
}

/* Run the compound command from line *ip to line end, whose trimmed closing
 * line `last` goes on after its kw-long keyword with a tail: redirections,
 * which apply to the whole command; a `|` and a pipeline, which it feeds
 * from a subshell; and an && or || list. Kept out of run_compound so that a
 * compound command without a tail does not carry the token array on the stack
 * while its body runs.
 */
static __attribute__((noinline)) int run_compound_tail(struct sh *sh,
                                                       struct sh_script *sc,
                                                       int *ip,
                                                       int end,
                                                       int mid,
                                                       char *last,
                                                       int kw)
{
    char *tail = last + kw + strspn(last + kw, " \t");
    char *pipe_at;
    const char *rest = NULL;
    struct sh_stmt *st = malloc(sizeof(*st) * SH_MAX_TOKENS);
    struct sh_token tok[SH_MAX_TOKENS];
    int pass[3] = {-1, -1, -1};
    int own[3] = {-1, -1, -1};
    int pfd[2] = {-1, -1};
    enum sh_op sep = SH_SEMI;
    int words;
    int ntok;
    int rc;

    /* The tail's first statement holds the redirections and the pipeline; an
     * AND-OR list may follow it.
     */
    ntok = st ? split_statements(tail, st, SH_MAX_TOKENS) : -1;
    if (ntok > 1) {
        size_t cut = strlen(st[0].text);

        sep = st[0].sep;
        rest = tail + cut + (sep == SH_SEMI ? 1 : 2);
        tail[cut] = '\0';
    }
    for (int k = 0; k < ntok; k++)
        free(st[k].text);
    free(st);
    if (ntok < 0) {
        fprintf(stderr, "sh: parse error\n");
        *ip = end;
        return 2;
    }
    pipe_at = (char *) sh_scan_find(tail, "|"); /* outside quotes */
    if (pipe_at) {
        *pipe_at++ = '\0';
        if (pipe(pfd) != 0) {
            fprintf(stderr, "sh: pipe: %s\n", strerror(errno));
            *ip = end;
            return 1;
        }
        (void) fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
        pass[1] = pfd[1]; /* so a 2>&1 duplicates the pipe */
    }

    sh->expand_abort = false;
    ntok = line_is_complete(sh, tail) ? tokenize(sh, tail, tok) : -1;
    if (ntok < 0 || sh->expand_abort) {
        rc = ntok < 0 ? 2 : sh->last_status;
    } else if ((words = sh_open_redirs(tok, 0, ntok, pass, own, NULL)) != 0) {
        if (words > 0)
            fprintf(stderr, "sh: syntax error after `%.*s'\n", kw, last);
        rc = words > 0 ? 2 : 1;
    } else if (pipe_at) {
        /* The compound runs in a subshell writing the pipe, not a job of its
         * own, while the shell runs the pipeline it feeds with the pipe as its
         * stdin.
         */
        struct sh_script sub = {0};
        pid_t pid = -1;
        bool jc = sh_jc;
        int saved[3];

        last[kw] = '\0'; /* the keyword; the subshell gets no tail */
        rc = 1;
        sh_jc = false;
        if (sh_sub_script(sc, *ip, end, last, &sub) == 0)
            rc = sh_spawn_sub(sh, 's', sub.line, sub.count, own[0],
                              own[1] != -1 ? own[1] : pfd[1], own[2], 0, &pid,
                              sub.line[0]);
        sh_jc = jc;
        script_free(&sub);

        /* The reader sees end of file once the subshell alone holds the write
         * end, also as a 2>&1 duplicate of it.
         */
        for (int k = 0; k < 3; k++) {
            close_if_needed(own[k]);
            own[k] = -1;
        }
        close(pfd[1]);
        pfd[1] = -1;
        if (rc == 0) {
            if (sh_redir_apply(pfd[0], -1, -1, saved) == 0)
                rc = run_text(sh, pipe_at);
            else
                rc = 1;
            sh_redir_restore(saved);
        }
        if (pid > 0)
            (void) waitpid(pid, NULL, 0);
    } else {
        int saved[3];

        if (sh_redir_apply(own[0], own[1], own[2], saved) != 0) {
            fprintf(stderr, "sh: redirection failed: %s\n", strerror(errno));
            rc = 1;
        } else {
            rc = run_block(sh, sc, ip, end, mid);
        }
        sh_redir_restore(saved);
    }
    if (ntok >= 0)
        token_free(tok, ntok);
    for (int k = 0; k < 3; k++)
        close_if_needed(own[k]);
    close_if_needed(pfd[0]);
    close_if_needed(pfd[1]);
    *ip = end;
    sh->last_status = rc;
    return rest && !sh->exit_requested ? run_list(sh, rest, sep, rc) : rc;
}

/* The & that ends the first statement of tail, the text after a closing keyword
 * (so the compound command is to run in the background), or NULL; *rest gets
 * what follows it.
 */
static char *tail_background(char *tail, const char **rest)
{
    struct sh_stmt *st = malloc(sizeof(*st) * SH_MAX_TOKENS);
    char *amp = NULL;
    int n = st ? split_statements(tail, st, SH_MAX_TOKENS) : -1;

    if (n > 0 && st[0].sep == SH_BG) {
        amp = tail + strlen(st[0].text);
        *rest = amp + 1;
    }
    for (int i = 0; i < n; i++)
        free(st[i].text);
    free(st);
    return amp;
}

/* Run the compound command opening at line *ip, and leave *ip at its last line.
 * A trailing `&` on its closing fi, done or esac runs the whole command in a
 * background subshell; any other text there is a tail for run_compound_tail.
 */
static __attribute__((noinline)) int run_compound_rest(struct sh *sh,
                                                       struct sh_script *sc,
                                                       int *ip,
                                                       int end,
                                                       int mid)
{
    char last[SH_LINE_MAX];
    struct sh_script sub = {0};
    const char *rest = "";
    char *amp;
    int kw = closer_len(sc->line[end] + strspn(sc->line[end], " \t"));
    int in;
    int rc;

    snprintf(last, sizeof last, "%s", sc->line[end]);
    trim(last);
    amp = tail_background(last + kw + strspn(last + kw, " \t"), &rest);
    if (!amp)
        return run_compound_tail(sh, sc, ip, end, mid, last, kw);

    in = sh_bg_stdin(sh);
    *amp = '\0';    /* the subshell gets the closing line without the & */
    sh_jc_pgid = 0; /* a job of its own */
    rc = sh_sub_script(sc, *ip, end, last, &sub) == 0
             ? sh_spawn_sub(sh, 's', sub.line, sub.count, in, -1, -1, 1, NULL,
                            sub.line[0])
             : 1;
    close_if_needed(in);
    script_free(&sub);
    *ip = end;
    sh->last_status = rc;
    rest += strspn(rest, " \t");
    return *rest != '\0' && !sh->exit_requested
               ? run_list(sh, rest, SH_SEMI, rc)
               : rc;
}

static int run_compound(struct sh *sh, struct sh_script *sc, int *ip, int to)
{
    int mid;
    int end = compound_end(sc, *ip, to, &mid);
    const char *tail;

    if (end < 0) {
        fprintf(stderr, "sh: syntax error: compound command not closed\n");
        return 2;
    }

    /* The common case, a bare closing keyword, runs without the stack the tail
     * and background paths need while the body runs.
     */
    tail = sc->line[end] + strspn(sc->line[end], " \t");
    tail += closer_len(tail);
    if (tail[strspn(tail, " \t\r")] == '\0')
        return run_block(sh, sc, ip, end, mid);
    return run_compound_rest(sh, sc, ip, end, mid);
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
        struct sh_script one = {0};
        int def;

        snprintf(line, sizeof(line), "%s", sc->line[i]);
        trim(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (starts_word(line, "then") || starts_word(line, "do") ||
            starts_word(line, "else") || starts_word(line, "fi") ||
            starts_word(line, "done") || starts_word(line, "esac"))
            continue;
        def = capture_func_def(sh, sc, &i, to, &one);
        if (def != 0) {
            if (def < 0)
                rc = 2;
            else if (one.count > 0)
                rc = run_script_range(sh, &one, 0, one.count);
            script_free(&one);
            continue;
        }
        if (starts_any(line, sh_compound)) {
            rc = run_compound(sh, sc, &i, to);
        } else {
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

/* Prepare loaded lines to run: continued lines joined, one-line compound
 * commands split.
 *
 * Returns 0, or -1 on allocation failure.
 */
static int script_prepare(struct sh_script *sc)
{
    return script_join_lines(sc) == 0 ? script_explode(sc) : -1;
}

static int run_file(struct sh *sh, const char *path)
{
    struct sh_script sc = {0};
    int rc;
    int fd;

    /* ipc-send: utils ->
     * roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring
     */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "sh: %s: %s\n", path, strerror(errno));
        return 1;
    }
    rc = load_script(fd, &sc) == 0 ? script_prepare(&sc) : -1;
    close(fd);
    if (rc != 0) {
        script_free(&sc);
        return 1;
    }
    rc = run_script_range(sh, &sc, 0, sc.count);
    script_free(&sc);
    return rc;
}

/* Run command text given as a string (sh -c, eval, the prompt, a subshell) as a
 * script of its lines, loaded as a file would be, unless it is one line too
 * long for a script line.
 */
static int run_text(struct sh *sh, const char *text)
{
    struct sh_script sc = {0};
    int rc = 0;

    if (strlen(text) >= SH_LINE_MAX && !strchr(text, '\n'))
        return run_line(sh, text);
    for (const char *p = text; rc == 0; p++) {
        const char *nl = strchr(p, '\n');
        char *l = dup_range(p, nl ? (size_t) (nl - p) : strlen(p));

        rc = l ? script_add(&sc, l) : -1;
        free(l);
        if (!nl)
            break;
        p = nl;
    }
    if (rc == 0)
        rc = script_prepare(&sc);
    rc = rc == 0 ? run_script_range(sh, &sc, 0, sc.count) : 1;
    script_free(&sc);
    return rc;
}

static int sh_read_line(struct sh *sh,
                        const char *prompt,
                        char *out,
                        size_t outsz)
{
    int rc;

    sh_trap_restart(sh, false);
    rc = sh_read_line_body(sh, prompt, out, outsz);
    sh_trap_restart(sh, true);
    return rc;
}

/* Drop the line end a read leaves on line. */
static void sh_chomp(char *line)
{
    size_t n = strlen(line);

    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
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
        sh_chomp(line);

        /* A quote or substitution still open goes on at a secondary prompt. */
        for (size_t cut;;) {
            char more[SH_LINE_MAX];
            bool blank;

            if (!line_open_join(line, &cut, &blank) ||
                sh_read_line(sh, "> ", more, sizeof(more)) != 0)
                break;
            sh_chomp(more);
            if (cut + blank + strlen(more) >= sizeof(line))
                break;
            line[cut] = '\n';
            strcpy(line + cut + blank, more);
        }
        trim(line);
        if (line[0] == '\0')
            continue;
        sh_add_history(sh, line);
        rc = run_text(sh, line);
    }
    if (sh_jc) {
        /* Hand the terminal back to the group we took it from (login, getty).
         */
        (void) tcsetpgrp(STDIN_FILENO, old_pgid);
        sh_jc = false;
    }
    sigaction(SIGINT, &sa_int_old, NULL);
    sigaction(SIGQUIT, &sa_quit_old, NULL);
    return rc;
}

/* Cursor over a state blob from sh_state_blob(). */
struct sh_blob {
    char *p;
    char *end;
    bool bad; /* a field was missing or malformed */
};

static char *blob_get(struct sh_blob *b)
{
    char *s = b->p;
    char *nul =
        b->p < b->end ? memchr(b->p, '\0', (size_t) (b->end - b->p)) : NULL;

    if (!nul) {
        b->bad = true;
        return NULL;
    }
    b->p = nul + 1;
    return s;
}

/* Read a number field in [lo, hi]. */
static long blob_getn(struct sh_blob *b, long lo, long hi)
{
    char *s = blob_get(b);
    char *e;
    long v;

    if (!s)
        return lo;
    errno = 0;
    v = strtol(s, &e, 10);
    if (e == s || *e != '\0' || errno || v < lo || v > hi) {
        b->bad = true;
        return lo;
    }
    return v;
}

/* Read a line-count field and then that many lines into *sc. */
static int blob_get_lines(struct sh_blob *b, struct sh_script *sc)
{
    long n = blob_getn(b, 0, INT_MAX);

    for (long i = 0; i < n && !b->bad; i++) {
        char *s = blob_get(b);

        if (s && script_add(sc, s) != 0)
            return -1;
    }
    return b->bad ? -1 : 0;
}

/* The child side of sh_spawn_sub: read the state on fd, restore it, run the
 * command it carries.
 *
 * Returns the command's exit status, or 2 when the state is unusable.
 */
static int sh_state_run(struct sh *sh, int fd)
{
    char *raw;
    size_t len;
    struct sh_blob b = {0};
    struct sh_script sc = {0};
    const char *kind;
    int rc = 2;
    bool ran = false;
    long n;

    if (read_all(fd, &raw, &len) != 0)
        goto out;
    close(fd);
    b.p = raw;
    b.end = raw + len;
    if (!blob_get(&b) || strcmp(raw, SH_STATE_MAGIC) != 0)
        goto out;
    sh->last_status = (int) blob_getn(&b, INT_MIN, INT_MAX);
    for (const char *o = blob_get(&b); o && *o; o++) {
        sh->opt_errexit |= *o == 'e';
        sh->opt_nounset |= *o == 'u';
        sh->opt_xtrace |= *o == 'x';
    }
    (void) umask((mode_t) blob_getn(&b, 0, 0777));
    {
        const char *cwd = blob_get(&b);

        /* Never run the command in some other directory. */
        if (cwd && *cwd && chdir(cwd) != 0) {
            fprintf(stderr, "sh: cd: %s: %s\n", cwd, strerror(errno));
            goto out;
        }
    }
    sh_top_pid = (pid_t) blob_getn(&b, 1, INT_MAX);
    sh_last_bg = (pid_t) blob_getn(&b, 0, INT_MAX);
    sh->arg0 = blob_get(&b);
    if (blob_get_lines(&b, &sc) != 0 ||
        sh_set_params(sh, sc.line, sc.count) != 0)
        goto out;
    script_free(&sc);
    n = blob_getn(&b, 0, INT_MAX);
    for (long i = 0; i < n && !b.bad; i++) {
        const char *name = blob_get(&b);

        if (blob_get_lines(&b, &sc) != 0 ||
            sh_add_func(sh, name, &sc, 0, sc.count) != 0)
            goto out;
        script_free(&sc);
    }

    /* The parent's traps: `trap` lists them, but a subshell does not run them
     * (POSIX 2.13); an ignored signal stays ignored across the exec.
     */
    n = blob_getn(&b, 0, SH_NTRAPS);
    for (long i = 0; i < n && !b.bad; i++) {
        int t = (int) blob_getn(&b, 0, SH_NTRAPS - 1);
        const char *action = blob_get(&b);

        if (action && !(sh->traps[t] = strdup(action)))
            goto out;
        sh->trap_inherited[t] = true;
    }
    n = blob_getn(&b, 0, SH_JOBS);
    for (long i = 0; i < n && !b.bad; i++) {
        struct sh_job *j = &sh_jobs[blob_getn(&b, 0, SH_JOBS - 1)];
        const char *cmd;

        j->pgid = (pid_t) blob_getn(&b, 0, INT_MAX);
        j->pid = (pid_t) blob_getn(&b, 1, INT_MAX);
        j->stopped = blob_getn(&b, 0, 1) != 0;
        cmd = blob_get(&b);
        snprintf(j->cmd, sizeof j->cmd, "%s", cmd ? cmd : "");
    }
    kind = blob_get(&b);
    if (blob_get_lines(&b, &sc) != 0 || !kind || sc.count < 1 || !kind[0] ||
        !strchr("acs", kind[0]) || kind[1] != '\0')
        goto out;
    ran = true;
    if (kind[0] == 'a')
        rc = run_line(sh, sc.line[0]);
    else if (kind[0] == 'c')
        rc = run_text(sh, sc.line[0]);
    else
        rc = run_script_range(sh, &sc, 0, sc.count);
out:
    if (!ran)
        fprintf(stderr, "sh: bad subshell state\n");
    script_free(&sc);
    sh->arg0 = NULL; /* points into raw */
    free(raw);
    return rc;
}

int toolbox_sh_main(int argc, char **argv)
{
    struct sh sh;
    int opt;
    const char *command = NULL;
    int state_fd = -1;

    memset(&sh, 0, sizeof(sh));
    sh.heredoc_fd = -1;
    optind = 1;
    while ((opt = getopt(argc, argv, "ic:S:")) != -1) {
        switch (opt) {
        case 'i':
            sh.interactive = true;
            break;
        case 'c':
            command = optarg;
            break;
        case 'S': /* internal: a subshell, its state on fd optarg */
            state_fd = atoi(optarg);
            break;
        default:
            fprintf(stderr, "usage: sh [-i] [-c COMMAND] [SCRIPT]\n");
            return 2;
        }
    }
    if (!getenv("PATH"))
        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    if (state_fd >= 0) {
        opt = sh_state_run(&sh, state_fd);
    } else if (command) {
        /* sh -c COMMAND [name [arg...]] */
        if (optind < argc) {
            sh.arg0 = argv[optind];
            sh_set_params(&sh, argv + optind + 1, argc - optind - 1);
        } else {
            sh.arg0 = argv[0];
        }
        opt = run_text(&sh, command);
    } else if (optind < argc) {
        /* sh SCRIPT [arg...] */
        sh.arg0 = argv[optind];
        sh_set_params(&sh, argv + optind + 1, argc - optind - 1);
        opt = run_file(&sh, argv[optind]);
    } else {
        sh.arg0 = argv[0];
        opt = run_interactive(&sh);
    }
    opt = sh_exit_trap(&sh, opt);
    for (int i = 0; i < SH_NTRAPS; i++)
        free(sh.traps[i]);
    sh_free_history(&sh);
    sh_free_funcs(&sh);
    for (int i = 0; i < sh.nparams; i++)
        free(sh.params[i]);
    free(sh.params);
    return opt;
}
