/*
 * toolbox vi - small full-screen editor applet.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "toolbox.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define VI_TABSTOP 8
#define VI_LOAD_CHUNK 1024

/* Upper bound on a numeric command count (e.g. 999j). Saturating here avoids
 * both size_t wrap and a multi-second loop when someone types an absurd repeat;
 * a million iterations of a boundary-pinned motion is already instant.
 */
#define VI_COUNT_MAX 1000000u

enum vi_key {
    VI_KEY_EOF = -1, /* read()<=0: EOF/error, vs a NUL (Ctrl+@) keystroke */
    VI_KEY_NULL = 0, /* interrupted read/no key */
    VI_KEY_ESC = 27,
    VI_KEY_BACKSPACE = 127,
    VI_KEY_ARROW_LEFT = 1000,
    VI_KEY_ARROW_RIGHT,
    VI_KEY_ARROW_UP,
    VI_KEY_ARROW_DOWN,
    VI_KEY_DELETE,
    VI_KEY_HOME,
    VI_KEY_END,
    VI_KEY_PAGE_UP,
    VI_KEY_PAGE_DOWN,
};

enum vi_redraw_level {
    VI_REDRAW_NONE = 0,
    VI_REDRAW_STATUS = 1,
    VI_REDRAW_LINE = 2,
    VI_REDRAW_SCREEN = 3,
};

enum vi_highlight {
    HL_DEFAULT = 0,
    HL_COMMENT,
    HL_STRING,
    HL_NUMBER,
    HL_KEYWORD,
    HL_TYPE,
};

/* Syntax color scheme: one SGR foreground code per highlight category. Edit a
 * value here to recolor that category everywhere. HL_DEFAULT is the terminal
 * default foreground (39).
 */
static const int vi_hl_color[] = {
    [HL_DEFAULT] = 39, /* default */
    [HL_COMMENT] = 32, /* green */
    [HL_STRING] = 36,  /* cyan */
    [HL_NUMBER] = 33,  /* yellow */
    [HL_KEYWORD] = 35, /* magenta */
    [HL_TYPE] = 34,    /* blue */
};

struct vi_line {
    char *data;
    size_t len;
    size_t cap;
};

struct vi_buffer {
    struct vi_line *line;
    size_t count;
    size_t cap;
};

struct vi {
    struct vi_buffer buf;
    char path[PATH_MAX];
    char status[160];
    char search[80];
    int search_backward;
    size_t count;
    char *yank_buf;
    size_t yank_len;
    int yank_linewise;
    size_t cx;
    size_t cy;
    size_t rowoff;
    size_t coloff;
    int rows;
    int cols;
    int dirty;
    int quit;
    int insert;
    int visual;     /* 0 = off, 1 = charwise (v), 2 = linewise (V) */
    size_t vsel_cx; /* selection anchor, valid while visual != 0 */
    size_t vsel_cy;
    int number;
    int raw_enabled;
    int last_key;
    int redraw;
    unsigned char *hl_buf; /* reusable highlight scratch, grows on demand */
    size_t hl_cap;
    struct termios saved_termios;
};

static void vi_request_redraw(struct vi *vi, int level)
{
    if (level > vi->redraw)
        vi->redraw = level;
}

static void vi_status(struct vi *vi, const char *msg)
{
    snprintf(vi->status, sizeof(vi->status), "%s", msg ? msg : "");
    vi_request_redraw(vi, VI_REDRAW_STATUS);
}

static void vi_die(const char *msg)
{
    fprintf(stderr, "vi: %s: %s\n", msg, strerror(errno));
}

static void vi_line_free(struct vi_line *line)
{
    free(line->data);
    line->data = NULL;
    line->len = 0;
    line->cap = 0;
}

static int vi_line_reserve(struct vi_line *line, size_t cap)
{
    char *data;

    if (cap <= line->cap)
        return 0;
    data = realloc(line->data, cap);
    if (!data)
        return -1;
    line->data = data;
    line->cap = cap;
    return 0;
}

static int vi_line_set(struct vi_line *line, const char *data, size_t len)
{
    if (vi_line_reserve(line, len + 1) != 0)
        return -1;
    if (len > 0)
        memcpy(line->data, data, len);
    line->data[len] = '\0';
    line->len = len;
    return 0;
}

static int vi_line_insert_char(struct vi_line *line, size_t at, int c)
{
    if (at > line->len)
        at = line->len;
    if (vi_line_reserve(line, line->len + 2) != 0)
        return -1;
    memmove(line->data + at + 1, line->data + at, line->len - at + 1);
    line->data[at] = (char) c;
    line->len++;
    return 0;
}

static void vi_line_delete_char(struct vi_line *line, size_t at)
{
    if (at >= line->len)
        return;
    memmove(line->data + at, line->data + at + 1, line->len - at);
    line->len--;
}

static void vi_buffer_free(struct vi_buffer *buf)
{
    for (size_t i = 0; i < buf->count; i++)
        vi_line_free(&buf->line[i]);
    free(buf->line);
    buf->line = NULL;
    buf->count = 0;
    buf->cap = 0;
}

static int vi_buffer_reserve(struct vi_buffer *buf, size_t cap)
{
    struct vi_line *line;

    if (cap <= buf->cap)
        return 0;
    if (cap > SIZE_MAX / sizeof(*line)) /* cap * sizeof would wrap */
        return -1;
    line = realloc(buf->line, cap * sizeof(*line));
    if (!line)
        return -1;
    memset(line + buf->cap, 0, (cap - buf->cap) * sizeof(*line));
    buf->line = line;
    buf->cap = cap;
    return 0;
}

static int vi_insert_line(struct vi *vi,
                          size_t at,
                          const char *data,
                          size_t len)
{
    struct vi_line nl;

    if (at > vi->buf.count)
        at = vi->buf.count;

    /* Build the new line first: if the copy or the array growth fails, the
     * buffer is left completely untouched (no half-shifted array, no leaked or
     * dropped line). Only once everything has succeeded do we splice it in.
     */
    memset(&nl, 0, sizeof(nl));
    if (vi_line_set(&nl, data, len) != 0)
        return -1;
    if (vi->buf.count == vi->buf.cap) {
        size_t cap;
        if (vi->buf.cap >
            SIZE_MAX / 2) { /* doubling would wrap to a smaller cap */
            vi_line_free(&nl);
            return -1;
        }
        cap = vi->buf.cap == 0 ? 16 : vi->buf.cap * 2;
        if (vi_buffer_reserve(&vi->buf, cap) != 0) {
            vi_line_free(&nl);
            return -1;
        }
    }
    memmove(&vi->buf.line[at + 1], &vi->buf.line[at],
            (vi->buf.count - at) * sizeof(*vi->buf.line));
    vi->buf.line[at] = nl;
    vi->buf.count++;
    return 0;
}

static int vi_delete_line(struct vi *vi, size_t at)
{
    if (at >= vi->buf.count)
        return -1;
    if (vi->buf.count == 1) {
        if (vi_line_set(&vi->buf.line[0], "", 0) != 0)
            return -1;
        vi->cx = 0;
        vi->cy = 0;
        vi->redraw = VI_REDRAW_SCREEN;
        return 0;
    }
    vi_line_free(&vi->buf.line[at]);
    memmove(&vi->buf.line[at], &vi->buf.line[at + 1],
            (vi->buf.count - at - 1) * sizeof(*vi->buf.line));
    vi->buf.count--;
    memset(&vi->buf.line[vi->buf.count], 0, sizeof(vi->buf.line[0]));
    if (vi->cy >= vi->buf.count)
        vi->cy = vi->buf.count - 1;
    if (vi->cx > vi->buf.line[vi->cy].len)
        vi->cx = vi->buf.line[vi->cy].len;
    vi_request_redraw(vi, VI_REDRAW_SCREEN);
    return 0;
}

static int vi_load(struct vi *vi)
{
    FILE *fp;
    char chunk[VI_LOAD_CHUNK];
    char *line = NULL;
    size_t len = 0;
    size_t cap = 0;
    int saw_any = 0;

    if (vi->path[0] == '\0')
        return vi_insert_line(vi, 0, "", 0);
    /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
    fp = fopen(vi->path, "r");
    if (!fp) {
        if (errno == ENOENT)
            return vi_insert_line(vi, 0, "", 0);
        vi_die(vi->path);
        return -1;
    }
    while (fgets(chunk, sizeof(chunk), fp)) {
        size_t part = strlen(chunk);
        int ended = part > 0 && chunk[part - 1] == '\n';

        if (ended)
            part--;
        if (len + part + 1 > cap) {
            size_t new_cap = cap == 0 ? 128 : cap * 2;
            char *new_line;
            while (new_cap < len + part + 1)
                new_cap *= 2;
            new_line = realloc(line, new_cap);
            if (!new_line) {
                fclose(fp);
                free(line);
                return -1;
            }
            line = new_line;
            cap = new_cap;
        }
        if (part > 0)
            memcpy(line + len, chunk, part);
        len += part;
        saw_any = 1;
        if (ended) {
            if (vi_insert_line(vi, vi->buf.count, line ? line : "", len) != 0) {
                fclose(fp);
                free(line);
                return -1;
            }
            len = 0;
        }
    }
    if (ferror(fp)) {
        vi_die(vi->path);
        fclose(fp);
        free(line);
        return -1;
    }
    if (len > 0 || !saw_any) {
        if (vi_insert_line(vi, vi->buf.count, line ? line : "", len) != 0) {
            fclose(fp);
            free(line);
            return -1;
        }
    }
    fclose(fp);
    free(line);
    return 0;
}

static int vi_save(struct vi *vi)
{
    FILE *fp;

    if (vi->path[0] == '\0') {
        vi_status(vi, "No file name");
        return -1;
    }

    /* TODO: switch to a temp + rename() once libc grows a rename() wrapper.
     * Today this truncates in place; a half-written file replaces the original.
     * Acceptable until RTMux libc lands rename().
     */
    /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
    fp = fopen(vi->path, "w");
    if (!fp) {
        snprintf(vi->status, sizeof(vi->status), "%s: %s", vi->path,
                 strerror(errno));
        vi->redraw = VI_REDRAW_STATUS;
        return -1;
    }
    for (size_t i = 0; i < vi->buf.count; i++) {
        if (fwrite(vi->buf.line[i].data, 1, vi->buf.line[i].len, fp) !=
            vi->buf.line[i].len) {
            snprintf(vi->status, sizeof(vi->status), "write failed: %s",
                     strerror(errno));
            vi->redraw = VI_REDRAW_STATUS;
            fclose(fp);
            return -1;
        }
        fputc('\n', fp);
    }
    if (fclose(fp) != 0) {
        snprintf(vi->status, sizeof(vi->status), "close failed: %s",
                 strerror(errno));
        vi->redraw = VI_REDRAW_STATUS;
        return -1;
    }
    vi->dirty = 0;
    snprintf(vi->status, sizeof(vi->status), "%u lines written",
             (unsigned) vi->buf.count);
    vi->redraw = VI_REDRAW_STATUS;
    return 0;
}

/* The signal handler needs to reach the editor's saved termios from outside the
 * struct vi. Track via a file-scope pointer set/cleared around vi_run.
 */
static struct vi *vi_signal_target;

/* Set by SIGWINCH, drained in the main loop so re-detection (which reads from
 * the tty) happens outside async-signal context.
 */
static volatile sig_atomic_t vi_winch_pending;

static void vi_restore_terminal(struct vi *vi)
{
    if (!vi)
        return;
    if (vi->raw_enabled)
        tcsetattr(STDIN_FILENO, TCSANOW, &vi->saved_termios);
    vi->raw_enabled = 0;

    /* Leave the alt-screen and re-show the cursor — same sequence vi_run()
     * emits on normal exit.
     */
    (void) !write(STDOUT_FILENO, "\x1b[?1049l\x1b[?25h", 14);
}

static void vi_signal_handler(int sig)
{
    vi_restore_terminal(vi_signal_target);
    /* Re-raise with default disposition so the parent observes the signal. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void vi_winch_handler(int sig)
{
    (void) sig;
    vi_winch_pending = 1;
}

static void vi_install_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = vi_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = vi_winch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags =
        0; /* stays installed; interrupts read() so resize reflows now */
    sigaction(SIGWINCH, &sa, NULL);
}

static void vi_enable_raw(struct vi *vi)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &vi->saved_termios) != 0)
        return;
    raw = vi->saved_termios;
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t) ~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
        vi->raw_enabled = 1;
}

static void vi_disable_raw(struct vi *vi)
{
    if (vi->raw_enabled)
        tcsetattr(STDIN_FILENO, TCSANOW, &vi->saved_termios);
    vi->raw_enabled = 0;
}

static int vi_env_size(const char *name, int min, int *out)
{
    const char *env = getenv(name);
    char *end;
    long value;

    if (!env)
        return 0;
    value = strtol(env, &end, 10);
    if (*end != '\0' || value < min || value >= 1000)
        return 0;
    *out = (int) value;
    return 1;
}

static void vi_flush(void); /* defined with the output buffer below */

static int vi_query_terminal_size(struct vi *vi, int *rows, int *cols)
{
    struct termios saved;
    struct termios query;
    char buf[32];
    char *p;
    char *end;
    int pos = 0;
    int out_rows;
    int out_cols;

    if (!vi->raw_enabled || tcgetattr(STDIN_FILENO, &saved) != 0)
        return 0;
    query = saved;
    query.c_cc[VMIN] = 0;
    query.c_cc[VTIME] = 2; /* up to ~0.2s for the reply to round-trip */
    if (tcsetattr(STDIN_FILENO, TCSANOW, &query) != 0)
        return 0;
    vi_flush(); /* don't leave buffered draw output behind the probe */
    /* Park the cursor at the far corner and ask where it landed. No DECSC/DECRC
     * (ESC 7 / ESC 8): terminals that ignore them print a literal '7'/'8'. The
     * caller repaints, which puts the cursor back where it belongs.
     */
    (void) !write(STDOUT_FILENO, "\033[999;999H\033[6n", 14);
    while (pos < (int) sizeof(buf) - 1) {
        char c;

        if (read(STDIN_FILENO, &c, 1) != 1)
            break;
        buf[pos++] = c;
        if (c == 'R')
            break;
    }
    (void) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    buf[pos] = '\0';

    /* Reply is ESC [ rows ; cols R. Try each ESC in the buffer so leading noise
     * (bytes from another console writer, a stray escape) doesn't defeat
     * detection when the real CPR reply is present.
     */
    for (p = buf; (p = strchr(p, '\033')) != NULL; p++) {
        if (p[1] != '[')
            continue;
        out_rows = (int) strtol(p + 2, &end, 10);
        if (*end != ';')
            continue;
        out_cols = (int) strtol(end + 1, &end, 10);

        /* Clamp to the same sane ceiling as the $LINES/$COLUMNS path: a hostile
         * or garbled reply (e.g. ESC[999999999;999999999R) would otherwise make
         * vi_draw_rows iterate billions of times.
         */
        if (*end != 'R' || out_rows <= 2 || out_cols <= 0 || out_rows >= 1000 ||
            out_cols >= 1000)
            continue;
        *rows = out_rows;
        *cols = out_cols;
        return 1;
    }
    return 0;
}

static void vi_detect_window_size(struct vi *vi)
{
    int rows = 24;
    int cols = 80;

    /* Ask the real terminal via a CPR probe (serial consoles carry no window
     * size); fall back to $LINES/$COLUMNS, then 80x24.
     */
    if (!vi_query_terminal_size(vi, &rows, &cols)) {
        (void) vi_env_size("LINES", 3, &rows);
        (void) vi_env_size("COLUMNS", 1, &cols);
    }
    if (rows < 3)
        rows = 3;
    if (cols < 1)
        cols = 1;
    vi->rows = rows;
    vi->cols = cols;

    /* The probe parks the cursor at the corner and we don't restore it, so a
     * full repaint is always needed to put it back (and reflow on any resize).
     */
    vi_request_redraw(vi, VI_REDRAW_SCREEN);
}

static int vi_read_key(void)
{
    char c;
    char seq[3];
    ssize_t n;

    /* With VMIN=1 a 0-length read means end-of-input (e.g. `echo | vi`), not a
     * transient empty read, so treat 0 like an error and report terminal loss
     * instead of spinning at 100% CPU waiting for bytes that never come.
     */
    n = read(STDIN_FILENO, &c, 1);
    if (n < 0)
        return errno == EINTR ? VI_KEY_NULL : VI_KEY_EOF;
    if (n == 0)
        return VI_KEY_EOF;
    if (c != '\033')
        return (unsigned char) c;
    if (read(STDIN_FILENO, &seq[0], 1) != 1)
        return VI_KEY_ESC;
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
        return VI_KEY_ESC;
    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1)
                return VI_KEY_ESC;
            if (seq[2] == '~') {
                switch (seq[1]) {
                case '1':
                case '7':
                    return VI_KEY_HOME;
                case '3':
                    return VI_KEY_DELETE;
                case '4':
                case '8':
                    return VI_KEY_END;
                case '5':
                    return VI_KEY_PAGE_UP;
                case '6':
                    return VI_KEY_PAGE_DOWN;
                default:
                    break;
                }
            }
        } else {
            switch (seq[1]) {
            case 'A':
                return VI_KEY_ARROW_UP;
            case 'B':
                return VI_KEY_ARROW_DOWN;
            case 'C':
                return VI_KEY_ARROW_RIGHT;
            case 'D':
                return VI_KEY_ARROW_LEFT;
            case 'H':
                return VI_KEY_HOME;
            case 'F':
                return VI_KEY_END;
            default:
                break;
            }
        }
    }
    return VI_KEY_ESC;
}

/* Output is batched into this file-scope buffer and flushed once per refresh,
 * turning a per-glyph write() storm (thousands of IPC round-trips on a serial
 * console) into a single syscall. Single-threaded applet, so a global is fine.
 */
static char *vi_out_buf;
static size_t vi_out_len;
static size_t vi_out_cap;

static void vi_write_raw(const char *s, size_t len)
{
    /* Loop to absorb partial writes and EINTR (e.g. SIGWINCH); the kernel
     * console now streams a write of any length, so there is no per-write cap.
     */
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, s, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (n == 0)
            return;
        s += (size_t) n;
        len -= (size_t) n;
    }
}

static void vi_flush(void)
{
    vi_write_raw(vi_out_buf, vi_out_len);
    vi_out_len = 0;
}

static void vi_write(const char *s)
{
    size_t len = strlen(s);

    if (len > SIZE_MAX - vi_out_len) {
        /* Pathological: sum would wrap. Flush and emit directly. */
        vi_flush();
        vi_write_raw(s, len);
        return;
    }
    if (vi_out_len + len > vi_out_cap) {
        size_t cap = vi_out_cap ? vi_out_cap : 1024;
        while (cap < vi_out_len + len) {
            if (cap > SIZE_MAX / 2) { /* next double would wrap */
                cap = vi_out_len + len;
                break;
            }
            cap *= 2;
        }
        char *grown = realloc(vi_out_buf, cap);
        if (!grown) {
            /* Can't grow — flush what we have and emit this chunk directly. */
            vi_flush();
            vi_write_raw(s, len);
            return;
        }
        vi_out_buf = grown;
        vi_out_cap = cap;
    }
    memcpy(vi_out_buf + vi_out_len, s, len);
    vi_out_len += len;
}

static int vi_line_rx(const struct vi_line *line, size_t cx)
{
    int rx = 0;

    if (cx > line->len)
        cx = line->len;
    for (size_t i = 0; i < cx; i++) {
        if (line->data[i] == '\t')
            rx += (VI_TABSTOP - 1) - (rx % VI_TABSTOP) + 1;
        else
            rx++;
    }
    return rx;
}

static int vi_scroll(struct vi *vi)
{
    int rx;
    int screen_rows = vi->rows - 2;
    int gutter = vi->number ? 6 : 0;
    int text_cols = vi->cols - gutter;

    if (text_cols < 1)
        text_cols = 1;

    size_t old_rowoff = vi->rowoff;
    size_t old_coloff = vi->coloff;

    if ((int) vi->cy < (int) vi->rowoff)
        vi->rowoff = vi->cy;
    if ((int) vi->cy >= (int) vi->rowoff + screen_rows)
        vi->rowoff = vi->cy - (size_t) screen_rows + 1;
    rx = vi_line_rx(&vi->buf.line[vi->cy], vi->cx);
    if (rx < (int) vi->coloff)
        vi->coloff = (size_t) rx;
    if (rx >= (int) vi->coloff + text_cols)
        vi->coloff = (size_t) (rx - text_cols + 1);
    return vi->rowoff != old_rowoff || vi->coloff != old_coloff;
}

static int is_separator(int c)
{
    return isspace((unsigned char) c) || c == '\0' ||
           strchr(",.()+-/*=~%<>[];{}", c) != NULL;
}

static void vi_compute_highlight(const char *data,
                                 size_t len,
                                 unsigned char *hl)
{
    memset(hl, HL_DEFAULT, len);
    int prev_sep = 1;
    int in_string = 0;
    size_t i = 0;

    while (i < len) {
        char c = data[i];

        if (!in_string && i + 1 < len && data[i] == '/' && data[i + 1] == '/') {
            memset(hl + i, HL_COMMENT, len - i);
            break;
        }

        if (in_string) {
            hl[i] = HL_STRING;
            if (c == '\\' && i + 1 < len) {
                hl[i + 1] = HL_STRING;
                i += 2;
                continue;
            }
            if (c == in_string) {
                in_string = 0;
            }
            i++;
            prev_sep = 1;
            continue;
        } else if (c == '"' || c == '\'') {
            in_string = c;
            hl[i] = HL_STRING;
            i++;
            continue;
        }

        if (isdigit((unsigned char) c) && prev_sep) {
            while (i < len &&
                   (isdigit((unsigned char) data[i]) || data[i] == '.')) {
                hl[i] = HL_NUMBER;
                i++;
            }
            prev_sep = 1;
            continue;
        }

        if (prev_sep) {
            size_t wlen = 0;
            while (i + wlen < len && !is_separator(data[i + wlen])) {
                wlen++;
            }
            if (wlen > 0) {
                static const char *keywords[] = {
                    "switch", "if",    "while",  "for",   "break",   "continue",
                    "return", "else",  "struct", "union", "typedef", "static",
                    "enum",   "class", "case",   NULL};
                static const char *types[] = {
                    "int",      "long",   "double", "float",  "char",
                    "unsigned", "signed", "void",   "size_t", NULL};
                int matched = 0;
                for (int j = 0; keywords[j]; j++) {
                    if (wlen == strlen(keywords[j]) &&
                        strncmp(data + i, keywords[j], wlen) == 0) {
                        memset(hl + i, HL_KEYWORD, wlen);
                        i += wlen;
                        matched = 1;
                        break;
                    }
                }
                if (!matched) {
                    for (int j = 0; types[j]; j++) {
                        if (wlen == strlen(types[j]) &&
                            strncmp(data + i, types[j], wlen) == 0) {
                            memset(hl + i, HL_TYPE, wlen);
                            i += wlen;
                            matched = 1;
                            break;
                        }
                    }
                }
                if (matched) {
                    prev_sep = 1;
                    continue;
                }
            }
        }

        prev_sep = is_separator(c);
        i++;
    }
}

static int vi_pos_selected(struct vi *vi, size_t filerow, size_t i);

static void vi_draw_row(struct vi *vi, int y)
{
    char buf[64];
    int gutter = vi->number ? 6 : 0;
    int text_cols = vi->cols - gutter;

    if (text_cols < 1)
        text_cols = 1;

    size_t filerow = vi->rowoff + (size_t) y;
    snprintf(buf, sizeof(buf), "\x1b[%d;1H", y + 1);
    vi_write(buf);

    if (filerow >= vi->buf.count) {
        vi_write("~");
        vi_write("\x1b[K");
    } else {
        const struct vi_line *line = &vi->buf.line[filerow];
        int drawn = 0;
        int rx = 0;
        unsigned char *hl = NULL;

        if (line->len > 0) {
            if (vi->hl_cap < line->len) {
                unsigned char *grown = realloc(vi->hl_buf, line->len);
                if (grown) {
                    vi->hl_buf = grown;
                    vi->hl_cap = line->len;
                }
            }
            if (vi->hl_cap >= line->len) {
                hl = vi->hl_buf;
                vi_compute_highlight(line->data, line->len, hl);
            }
        }

        if (vi->number) {
            snprintf(buf, sizeof(buf), "%4u  ", (unsigned) (filerow + 1));
            vi_write(buf);
        }
        int current_color = vi_hl_color[HL_DEFAULT];
        int in_sel = 0;
        for (size_t i = 0; i < line->len && drawn < text_cols; i++) {
            char c = line->data[i];
            int width = c == '\t' ? VI_TABSTOP - (rx % VI_TABSTOP) : 1;

            if (rx + width <= (int) vi->coloff) {
                rx += width;
                continue;
            }

            int want_color = hl ? vi_hl_color[hl[i]] : vi_hl_color[HL_DEFAULT];
            if (want_color != current_color) {
                current_color = want_color;
                char esc[16];
                snprintf(esc, sizeof(esc), "\x1b[%dm", current_color);
                vi_write(esc);
            }

            int want_sel = vi->visual && vi_pos_selected(vi, filerow, i);
            if (want_sel != in_sel) {
                in_sel = want_sel;
                vi_write(in_sel ? "\x1b[7m" : "\x1b[27m");
            }

            if (c == '\t') {
                do {
                    vi_write(" ");
                    drawn++;
                } while (++rx % VI_TABSTOP != 0 && drawn < text_cols);
            } else if (!isprint((unsigned char) c)) {
                vi_write("?");
                drawn++;
                rx++;
            } else {
                char out[2] = {
                    [0] = c,
                    [1] = '\0',
                };
                vi_write(out);
                drawn++;
                rx++;
            }
        }
        if (in_sel)
            vi_write("\x1b[27m");
        if (current_color != vi_hl_color[HL_DEFAULT]) {
            vi_write("\x1b[39m");
        }
        vi_write("\x1b[K");
    }
}

static void vi_draw_rows(struct vi *vi)
{
    int screen_rows = vi->rows - 2;
    for (int y = 0; y < screen_rows; y++) {
        vi_draw_row(vi, y);
    }
}

static void vi_draw_status(struct vi *vi)
{
    char left[128];
    char right[64];
    int len;
    int right_len;
    int left_max;
    const char *pct;
    char buf[32];

    snprintf(buf, sizeof(buf), "\x1b[%d;1H", vi->rows - 1);
    vi_write(buf);

    if (vi->buf.count <= 1)
        pct = "All";
    else if (vi->cy == 0)
        pct = "Top";
    else if (vi->cy + 1 >= vi->buf.count)
        pct = "Bot";
    else {
        static char pctbuf[8];
        snprintf(pctbuf, sizeof(pctbuf), "%u%%",
                 (unsigned) ((vi->cy + 1) * 100 / vi->buf.count));
        pct = pctbuf;
    }

    vi_write("\x1b[7m");
    const char *mode = vi->insert        ? " -- INSERT --"
                       : vi->visual == 2 ? " -- VISUAL LINE --"
                       : vi->visual      ? " -- VISUAL --"
                                         : "";
    snprintf(left, sizeof(left), " %s%s%s",
             vi->path[0] != '\0' ? vi->path : "[No Name]",
             vi->dirty ? " [+]" : "", mode);
    snprintf(right, sizeof(right), " %s  %u,%u ", pct, (unsigned) (vi->cy + 1),
             (unsigned) (vi->cx + 1));
    right_len = (int) strlen(right);
    left_max = vi->cols - right_len;
    if (left_max < 0)
        left_max = 0;
    if ((int) strlen(left) > left_max)
        left[left_max < (int) sizeof(left) ? left_max
                                           : (int) sizeof(left) - 1] = '\0';
    len = (int) strlen(left);
    vi_write(left);
    while (len < left_max) {
        vi_write(" ");
        len++;
    }
    if (right_len <= vi->cols)
        vi_write(right);
    vi_write("\x1b[m");

    snprintf(buf, sizeof(buf), "\x1b[%d;1H", vi->rows);
    vi_write(buf);
    vi_write(vi->status);
    vi_write("\x1b[K");
}

static int vi_is_word(int c)
{
    return isalnum((unsigned char) c) || c == '_';
}

static int vi_is_blank(int c)
{
    return c == ' ' || c == '\t';
}

static void vi_word_forward(struct vi *vi)
{
    size_t row = vi->cy;
    size_t col = vi->cx;

    while (row < vi->buf.count) {
        struct vi_line *line = &vi->buf.line[row];

        if (col < line->len) {
            int word = vi_is_word(line->data[col]);

            while (col < line->len && !vi_is_blank(line->data[col]) &&
                   vi_is_word(line->data[col]) == word)
                col++;
            while (col < line->len && vi_is_blank(line->data[col]))
                col++;
            if (col < line->len) {
                vi->cy = row;
                vi->cx = col;
                return;
            }
        }
        row++;
        col = 0;
    }
}

static void vi_word_back(struct vi *vi)
{
    size_t row = vi->cy;
    size_t col = vi->cx;

    for (;;) {
        struct vi_line *line = &vi->buf.line[row];

        if (col > line->len)
            col = line->len;
        if (col > 0) {
            col--;
            while (col > 0 && vi_is_blank(line->data[col]))
                col--;
            if (!vi_is_blank(line->data[col])) {
                int word = vi_is_word(line->data[col]);

                while (col > 0 && !vi_is_blank(line->data[col - 1]) &&
                       vi_is_word(line->data[col - 1]) == word)
                    col--;
                vi->cy = row;
                vi->cx = col;
                return;
            }
        }
        if (row == 0)
            return;
        row--;
        col = vi->buf.line[row].len;
    }
}

static void vi_word_end(struct vi *vi)
{
    size_t row = vi->cy;
    size_t col = vi->cx + 1;

    while (row < vi->buf.count) {
        struct vi_line *line = &vi->buf.line[row];

        while (col < line->len && vi_is_blank(line->data[col]))
            col++;
        if (col < line->len) {
            int word = vi_is_word(line->data[col]);

            while (col + 1 < line->len && !vi_is_blank(line->data[col + 1]) &&
                   vi_is_word(line->data[col + 1]) == word)
                col++;
            vi->cy = row;
            vi->cx = col;
            return;
        }
        row++;
        col = 0;
    }
}

static void vi_move_cursor(struct vi *vi)
{
    char buf[64];
    int gutter = vi->number ? 6 : 0;
    int rx;
    int screen_cx;

    rx = vi_line_rx(&vi->buf.line[vi->cy], vi->cx);
    screen_cx = rx - (int) vi->coloff + gutter + 1;
    if (screen_cx < 1)
        screen_cx = 1;
    snprintf(buf, sizeof(buf), "\x1b[%u;%uH",
             (unsigned) (vi->cy - vi->rowoff + 1), (unsigned) screen_cx);
    vi_write(buf);
}

static void vi_refresh(struct vi *vi)
{
    char buf[64];
    int scrolled;

    scrolled = vi_scroll(vi);
    if (scrolled) {
        vi->redraw = VI_REDRAW_SCREEN;
    }

    if (vi->redraw == VI_REDRAW_NONE) {
        snprintf(buf, sizeof(buf), "\x1b[%u;1H", (unsigned) (vi->rows - 1));
        vi_write(buf);
        vi_draw_status(vi);
        vi_move_cursor(vi);
        vi_flush();
        return;
    }

    if (vi->redraw == VI_REDRAW_SCREEN) {
        vi_write("\x1b[H");
        vi_draw_rows(vi);
        vi_draw_status(vi);
    } else if (vi->redraw == VI_REDRAW_LINE) {
        int y = (int) vi->cy - (int) vi->rowoff;
        int screen_rows = vi->rows - 2;
        if (y >= 0 && y < screen_rows) {
            vi_draw_row(vi, y);
        }
        vi_draw_status(vi);
    } else if (vi->redraw == VI_REDRAW_STATUS) {
        vi_draw_status(vi);
    }
    vi_move_cursor(vi);
    vi_flush();
    vi->redraw = VI_REDRAW_NONE;
}

static void vi_move(struct vi *vi, int key)
{
    struct vi_line *line = &vi->buf.line[vi->cy];
    int page = vi->rows > 4 ? vi->rows - 3 : 1;

    switch (key) {
    case VI_KEY_ARROW_LEFT:
    case 'h':
        if (vi->cx > 0) {
            vi->cx--;
        } else if (vi->cy > 0) {
            vi->cy--;
            vi->cx = vi->buf.line[vi->cy].len;
        }
        break;
    case VI_KEY_ARROW_RIGHT:
    case 'l':
        if (vi->cx < line->len) {
            vi->cx++;
        } else if (vi->cy + 1 < vi->buf.count) {
            vi->cy++;
            vi->cx = 0;
        }
        break;
    case VI_KEY_ARROW_UP:
    case 'k':
        if (vi->cy > 0)
            vi->cy--;
        break;
    case VI_KEY_ARROW_DOWN:
    case 'j':
        if (vi->cy + 1 < vi->buf.count)
            vi->cy++;
        break;
    case VI_KEY_HOME:
    case '0':
        vi->cx = 0;
        break;
    case VI_KEY_END:
    case '$':
        vi->cx = line->len;
        break;
    case VI_KEY_PAGE_UP:
        vi->cy = vi->cy > (size_t) page ? vi->cy - (size_t) page : 0;
        break;
    case VI_KEY_PAGE_DOWN:
        vi->cy += (size_t) page;
        if (vi->cy >= vi->buf.count)
            vi->cy = vi->buf.count - 1;
        break;
    default:
        break;
    }
    line = &vi->buf.line[vi->cy];
    if (vi->cx > line->len)
        vi->cx = line->len;
}

static void vi_first_nonblank(struct vi *vi)
{
    struct vi_line *line = &vi->buf.line[vi->cy];
    size_t cx = 0;

    while (cx < line->len && vi_is_blank(line->data[cx]))
        cx++;
    vi->cx = cx;
}

static int vi_insert_char(struct vi *vi, int c)
{
    if (vi_line_insert_char(&vi->buf.line[vi->cy], vi->cx, c) != 0)
        return -1;
    vi->cx++;
    vi->dirty = 1;
    vi->redraw = VI_REDRAW_LINE;
    return 0;
}

static int vi_insert_newline(struct vi *vi)
{
    struct vi_line *line = &vi->buf.line[vi->cy];
    const char *tail = line->data + vi->cx;
    size_t tail_len = line->len - vi->cx;

    if (vi_insert_line(vi, vi->cy + 1, tail, tail_len) != 0)
        return -1;
    line = &vi->buf.line[vi->cy];
    line->data[vi->cx] = '\0';
    line->len = vi->cx;
    vi->cy++;
    vi->cx = 0;
    vi->dirty = 1;
    vi_request_redraw(vi, VI_REDRAW_SCREEN);
    return 0;
}

static void vi_backspace(struct vi *vi)
{
    struct vi_line *line = &vi->buf.line[vi->cy];

    if (vi->cx > 0) {
        vi_line_delete_char(line, vi->cx - 1);
        vi->cx--;
        vi->dirty = 1;
        vi->redraw = VI_REDRAW_LINE;
    } else if (vi->cy > 0) {
        size_t old_len = vi->buf.line[vi->cy - 1].len;
        if (vi_line_reserve(&vi->buf.line[vi->cy - 1],
                            old_len + line->len + 1) != 0)
            return;
        memcpy(vi->buf.line[vi->cy - 1].data + old_len, line->data,
               line->len + 1);
        vi->buf.line[vi->cy - 1].len += line->len;
        vi_delete_line(vi, vi->cy);
        vi->cy--;
        vi->cx = old_len;
        vi->dirty = 1;
        vi->redraw = VI_REDRAW_SCREEN;
    }
}

static void vi_delete_char(struct vi *vi)
{
    struct vi_line *line = &vi->buf.line[vi->cy];

    if (vi->cx < line->len) {
        vi_line_delete_char(line, vi->cx);
        vi->dirty = 1;
        vi->redraw = VI_REDRAW_LINE;
    } else if (vi->cy + 1 < vi->buf.count) {
        struct vi_line *next = &vi->buf.line[vi->cy + 1];
        if (vi_line_reserve(line, line->len + next->len + 1) != 0)
            return;
        memcpy(line->data + line->len, next->data, next->len + 1);
        line->len += next->len;
        vi_delete_line(vi, vi->cy + 1);
        vi->dirty = 1;
        vi->redraw = VI_REDRAW_SCREEN;
    }
}

static void vi_join_line(struct vi *vi)
{
    struct vi_line *line = &vi->buf.line[vi->cy];
    struct vi_line *next;
    size_t old_len;
    int add_space;

    if (vi->cy + 1 >= vi->buf.count)
        return;
    next = &vi->buf.line[vi->cy + 1];
    old_len = line->len;
    add_space =
        old_len > 0 && next->len > 0 && !vi_is_blank(line->data[old_len - 1]);
    if (vi_line_reserve(line, old_len + (size_t) add_space + next->len + 1) !=
        0)
        return;
    if (add_space)
        line->data[line->len++] = ' ';
    memcpy(line->data + line->len, next->data, next->len + 1);
    line->len += next->len;
    vi_delete_line(vi, vi->cy + 1);
    vi->cx = old_len;
    vi->dirty = 1;
    vi->redraw = VI_REDRAW_SCREEN;
}

static void vi_prompt(struct vi *vi, const char *prefix, char *out, size_t len)
{
    size_t pos = 0;

    out[0] = '\0';
    for (;;) {
        int c;
        char line[200];

        snprintf(line, sizeof(line), "%s%s", prefix, out);
        vi_status(vi, line);
        vi_refresh(vi);
        c = vi_read_key();
        if (c == VI_KEY_NULL)
            continue;
        if (c == VI_KEY_EOF) {
            vi->quit = 1;
            out[0] = '\0';
            vi_status(vi, "");
            return;
        }
        if (c == '\r' || c == '\n') {
            vi_status(vi, "");
            return;
        }
        if (c == VI_KEY_ESC) {
            out[0] = '\0';
            vi_status(vi, "");
            return;
        }
        if (c == VI_KEY_BACKSPACE || c == '\b') {
            if (pos > 0)
                out[--pos] = '\0';
            continue;
        }
        if (c >= 0 && c <= UCHAR_MAX && isprint(c) && pos + 1 < len) {
            out[pos++] = (char) c;
            out[pos] = '\0';
        }
    }
}

static void vi_search_step(struct vi *vi, int forward)
{
    if (vi->search[0] == '\0') {
        vi_status(vi, "No previous search");
        return;
    }

    size_t plen = strlen(vi->search);

    if (forward) {
        for (size_t offset = 0; offset <= vi->buf.count; offset++) {
            size_t r = (vi->cy + offset) % vi->buf.count;
            struct vi_line *line = &vi->buf.line[r];
            if (line->len < plen)
                continue;
            size_t c_start = 0;
            size_t c_end = line->len - plen;

            if (offset == 0) {
                c_start = vi->cx + 1;
            } else if (offset == vi->buf.count) {
                if (vi->cx >= plen)
                    c_end = vi->cx - 1;
                else
                    continue;
                c_end = vi->cx < c_end ? vi->cx : c_end;
            }

            if (c_start <= c_end) {
                for (size_t c = c_start; c <= c_end; c++) {
                    if (strncmp(line->data + c, vi->search, plen) == 0) {
                        vi->cy = r;
                        vi->cx = c;
                        if (offset > 0)
                            vi_status(vi, "Search wrapped");
                        vi->redraw = VI_REDRAW_SCREEN;
                        return;
                    }
                }
            }
        }
    } else {
        for (size_t offset = 0; offset <= vi->buf.count; offset++) {
            size_t r = (vi->cy + vi->buf.count - (offset % vi->buf.count)) %
                       vi->buf.count;
            struct vi_line *line = &vi->buf.line[r];
            if (line->len < plen)
                continue;

            size_t c_start = line->len - plen;
            size_t c_end = 0;

            if (offset == 0) {
                if (vi->cx > 0) {
                    c_start = vi->cx - 1;
                    if (c_start > line->len - plen)
                        c_start = line->len - plen;
                } else {
                    continue;
                }
            } else if (offset == vi->buf.count) {
                c_end = vi->cx + 1;
                if (c_end > line->len - plen)
                    continue;
            }

            if (c_start >= c_end) {
                for (size_t i = 0; i <= c_start - c_end; i++) {
                    size_t c = c_start - i;
                    if (strncmp(line->data + c, vi->search, plen) == 0) {
                        vi->cy = r;
                        vi->cx = c;
                        if (offset > 0)
                            vi_status(vi, "Search wrapped");
                        vi->redraw = VI_REDRAW_SCREEN;
                        return;
                    }
                }
            }
        }
    }
    vi_status(vi, "Pattern not found");
}

static void vi_search(struct vi *vi, int forward)
{
    char needle[sizeof(vi->search)];
    const char *prompt = forward ? "/" : "?";

    vi_prompt(vi, prompt, needle, sizeof(needle));
    if (needle[0] == '\0')
        return;
    snprintf(vi->search, sizeof(vi->search), "%s", needle);
    vi->search_backward = !forward;
    vi_search_step(vi, forward);
}

static void vi_yank_text(struct vi *vi,
                         const char *text,
                         size_t len,
                         int linewise)
{
    char *buf = malloc(len + 1);
    if (!buf)
        return; /* keep the existing register intact on failure */
    memcpy(buf, text, len);
    buf[len] = '\0';
    free(vi->yank_buf);
    vi->yank_buf = buf;
    vi->yank_len = len;
    vi->yank_linewise = linewise;
}

static void vi_yank_line(struct vi *vi, size_t count)
{
    if (vi->buf.count == 0)
        return;
    size_t start = vi->cy;
    if (count > vi->buf.count - start) /* clamp before start + count wraps */
        count = vi->buf.count - start;
    size_t end = start + count;

    size_t total_len = 0;
    for (size_t i = start; i < end; i++) {
        size_t line_len = vi->buf.line[i].len;
        if (total_len > SIZE_MAX - line_len - 1)
            return; /* buffer too large to yank */
        total_len += line_len + 1;
    }

    char *buf = malloc(total_len + 1);
    if (!buf)
        return;

    size_t pos = 0;
    for (size_t i = start; i < end; i++) {
        memcpy(buf + pos, vi->buf.line[i].data, vi->buf.line[i].len);
        pos += vi->buf.line[i].len;
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';

    vi_yank_text(vi, buf, total_len, 1);
    free(buf);

    char msg[80];
    snprintf(msg, sizeof(msg), "%u lines yanked", (unsigned) (end - start));
    vi_status(vi, msg);
}

static void vi_paste(struct vi *vi, int after)
{
    if (!vi->yank_buf || vi->yank_len == 0) {
        vi_status(vi, "Nothing in register");
        return;
    }

    if (vi->yank_linewise) {
        size_t insert_at = after ? vi->cy + 1 : vi->cy;
        const char *p = vi->yank_buf;
        const char *end = vi->yank_buf + vi->yank_len;
        size_t inserted = 0;

        while (p < end) {
            const char *next_nl = memchr(p, '\n', (size_t) (end - p));
            size_t len = next_nl ? (size_t) (next_nl - p) : (size_t) (end - p);
            if (vi_insert_line(vi, insert_at, p, len) != 0)
                break; /* out of memory — stop, don't leave a gap */
            insert_at++;
            inserted++;
            p += len;
            if (next_nl)
                p++;
        }
        if (inserted > 0) {
            vi->dirty = 1;
            vi->redraw = VI_REDRAW_SCREEN;
            vi->cy = after ? vi->cy + 1 : vi->cy;
            vi->cx = 0;
            char msg[80];
            snprintf(msg, sizeof(msg), "%u lines pasted", (unsigned) inserted);
            vi_status(vi, msg);
        }
    } else if (!memchr(vi->yank_buf, '\n', vi->yank_len)) {
        /* Single-line charwise register: splice the bytes into the line. */
        struct vi_line *line = &vi->buf.line[vi->cy];
        size_t paste_at = after ? vi->cx + 1 : vi->cx;
        if (paste_at > line->len)
            paste_at = line->len;

        size_t inserted = 0;
        for (size_t i = 0; i < vi->yank_len; i++) {
            if (vi_line_insert_char(line, paste_at + i, vi->yank_buf[i]) != 0)
                break;
            inserted++;
        }
        if (inserted > 0) {
            vi->cx = paste_at + inserted - 1;
            vi->dirty = 1;
            vi_request_redraw(vi, VI_REDRAW_LINE);
        }
    } else {
        /* Multi-line charwise register (from a visual selection spanning rows):
         * split the current line at the paste point and thread the register's
         * newline-separated segments across new lines, so the trailing text of
         * the original line follows the register's last segment.
         */
        struct vi_line *line = &vi->buf.line[vi->cy];
        size_t paste_at = after ? vi->cx + 1 : vi->cx;
        if (paste_at > line->len)
            paste_at = line->len;

        size_t tail_len = line->len - paste_at;
        char *tail = malloc(tail_len + 1);
        if (!tail)
            return;
        memcpy(tail, line->data + paste_at, tail_len);

        const char *y = vi->yank_buf;
        const char *yend = y + vi->yank_len;
        const char *nl = memchr(y, '\n', (size_t) (yend - y));
        size_t seg0 = (size_t) (nl - y);

        /* Current line becomes head[0..paste_at) + first segment. */
        char *first = malloc(paste_at + seg0 + 1);
        if (!first) {
            free(tail);
            return;
        }
        memcpy(first, line->data, paste_at);
        memcpy(first + paste_at, y, seg0);
        if (vi_line_set(line, first, paste_at + seg0) != 0) {
            free(first);
            free(tail);
            return;
        }
        free(first);

        size_t at = vi->cy + 1;
        size_t last_cy = vi->cy;
        y = nl + 1;
        while (y <= yend) {
            nl = memchr(y, '\n', (size_t) (yend - y));
            if (nl) {
                if (vi_insert_line(vi, at, y, (size_t) (nl - y)) != 0)
                    break;
                last_cy = at++;
                y = nl + 1;
                continue;
            }
            /* Final segment carries the saved tail of the original line. */
            size_t seg = (size_t) (yend - y);
            char *last = malloc(seg + tail_len + 1);
            if (last) {
                memcpy(last, y, seg);
                memcpy(last + seg, tail, tail_len);
                if (vi_insert_line(vi, at, last, seg + tail_len) == 0)
                    last_cy = at;
                free(last);
            }
            break;
        }
        free(tail);
        vi->cy = last_cy;
        vi->cx = 0;
        vi->dirty = 1;
        vi->redraw = VI_REDRAW_SCREEN;
    }
}

/* Full-screen key reference shown by :help. Draw it over the alt-screen, wait
 * for any key, then ask for a full repaint to restore the editor.
 */
static void vi_show_help(struct vi *vi)
{
    static const char *const lines[] = {
        "  toolbox vi \342\200\224 key reference",
        "",
        "  Movement",
        "    h j k l / arrows    left, down, up, right",
        "    w  b  e             word: forward, back, end",
        "    0  ^  $             line: start, first word, end",
        "    g  G                file: top, bottom",
        "    %                   jump to matching bracket",
        "    PgUp PgDn           page up, page down",
        "  Insert",
        "    i I   a A   o O     at cursor/start, after/end, open below/above",
        "  Edit",
        "    x  X                delete char at / before cursor",
        "    dd  D   C           delete line / to end, change to end",
        "    r  ~   J            replace char, toggle case, join line",
        "    yy  p  P            yank line, paste after / before",
        "  Search",
        "    /  ?    n  N        find forward / back, next / prev",
        "  Files",
        "    :w  :wq  :q  :q!    write, write-quit, quit, force quit",
        "    :set number         toggle line numbers",
        "",
        "  Press any key to return",
    };
    char buf[32];
    int c;
    size_t n = sizeof(lines) / sizeof(lines[0]);

    vi_write("\x1b[2J\x1b[H");
    for (size_t i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "\x1b[%d;1H", (int) (i + 1));
        vi_write(buf);
        vi_write(lines[i]);
    }
    vi_flush();
    while ((c = vi_read_key()) == VI_KEY_NULL)
        ;
    if (c == VI_KEY_EOF)
        vi->quit = 1;
    vi_request_redraw(vi, VI_REDRAW_SCREEN);
}

static void vi_ex(struct vi *vi)
{
    char cmd[80];
    const char *arg = "";
    char cmd_name[20];
    size_t i = 0;

    vi_prompt(vi, ":", cmd, sizeof(cmd));
    if (cmd[0] == '\0')
        return;

    while (cmd[i] != '\0' && cmd[i] != ' ' && i < sizeof(cmd_name) - 1) {
        cmd_name[i] = cmd[i];
        i++;
    }
    cmd_name[i] = '\0';
    while (cmd[i] == ' ')
        i++;
    if (cmd[i] != '\0')
        arg = cmd + i;

    if (!strcmp(cmd_name, "w")) {
        if (*arg != '\0') {
            snprintf(vi->path, sizeof(vi->path), "%s", arg);
        }
        vi_save(vi);
    } else if (!strcmp(cmd_name, "q")) {
        if (vi->dirty) {
            vi_status(vi, "No write since last change (:q! overrides)");
        } else {
            vi->quit = 1;
        }
    } else if (!strcmp(cmd_name, "q!")) {
        vi->quit = 1;
    } else if (!strcmp(cmd_name, "wq") || !strcmp(cmd_name, "x")) {
        if (*arg != '\0') {
            snprintf(vi->path, sizeof(vi->path), "%s", arg);
        }
        if (vi_save(vi) == 0)
            vi->quit = 1;
    } else if (!strcmp(cmd, "set number") || !strcmp(cmd, "set nu")) {
        vi->number = 1;
        vi_request_redraw(vi, VI_REDRAW_SCREEN);
    } else if (!strcmp(cmd, "set nonumber") || !strcmp(cmd, "set nonu")) {
        vi->number = 0;
        vi_request_redraw(vi, VI_REDRAW_SCREEN);
    } else if (!strcmp(cmd_name, "help")) {
        vi_show_help(vi);
    } else {
        vi_status(vi, "Unknown command");
    }
}

static void vi_match_bracket(struct vi *vi)
{
    struct vi_line *line = &vi->buf.line[vi->cy];
    if (vi->cx >= line->len)
        return;

    char current = line->data[vi->cx];
    char target = '\0';
    int direction = 0;

    if (current == '(') {
        target = ')';
        direction = 1;
    } else if (current == '[') {
        target = ']';
        direction = 1;
    } else if (current == '{') {
        target = '}';
        direction = 1;
    } else if (current == ')') {
        target = '(';
        direction = -1;
    } else if (current == ']') {
        target = '[';
        direction = -1;
    } else if (current == '}') {
        target = '{';
        direction = -1;
    }

    if (direction == 0)
        return;

    int depth = 0;
    size_t r = vi->cy;
    size_t c = vi->cx;

    while (1) {
        if (direction > 0) {
            c++;
            if (c >= vi->buf.line[r].len) {
                r++;
                if (r >= vi->buf.count)
                    break;
                c = 0;
            }
        } else {
            if (c == 0) {
                if (r == 0)
                    break;
                r--;
                c = vi->buf.line[r].len;
            }
            if (c > 0)
                c--;
        }

        if (c < vi->buf.line[r].len) {
            char ch = vi->buf.line[r].data[c];
            if (ch == current) {
                depth++;
            } else if (ch == target) {
                if (depth == 0) {
                    vi->cy = r;
                    vi->cx = c;
                    vi_request_redraw(vi, VI_REDRAW_STATUS);
                    return;
                }
                depth--;
            }
        }
    }
}

static size_t vi_get_and_reset_count(struct vi *vi)
{
    size_t count = vi->count > 0 ? vi->count : 1;
    vi->count = 0;
    return count;
}

/* Cursor motions shared by normal and visual mode.
 *
 * Returns true when `c` was a motion key (count already consumed and applied).
 * `G` and word/line motions live here; `gg` stays in vi_normal_key because it
 * needs last_key state.
 */
static int vi_motion_key(struct vi *vi, int c)
{
    switch (c) {
    case 'h':
    case 'j':
    case 'k':
    case 'l':
    case '0':
    case '$':
    case VI_KEY_ARROW_LEFT:
    case VI_KEY_ARROW_RIGHT:
    case VI_KEY_ARROW_UP:
    case VI_KEY_ARROW_DOWN:
    case VI_KEY_HOME:
    case VI_KEY_END:
    case VI_KEY_PAGE_UP:
    case VI_KEY_PAGE_DOWN: {
        size_t count = vi_get_and_reset_count(vi);
        for (size_t i = 0; i < count; i++)
            vi_move(vi, c);
        return 1;
    }
    case '^':
        vi_get_and_reset_count(vi);
        vi_first_nonblank(vi);
        return 1;
    case '%':
        vi_get_and_reset_count(vi);
        vi_match_bracket(vi);
        return 1;
    case 'w':
    case 'b':
    case 'e': {
        size_t count = vi_get_and_reset_count(vi);
        for (size_t i = 0; i < count; i++) {
            if (c == 'w')
                vi_word_forward(vi);
            else if (c == 'b')
                vi_word_back(vi);
            else
                vi_word_end(vi);
        }
        return 1;
    }
    case 'G':
        if (vi->count > 0) {
            size_t target = vi->count - 1;
            if (target >= vi->buf.count)
                target = vi->buf.count - 1;
            vi->cy = target;
            vi->count = 0;
        } else {
            vi->cy = vi->buf.count - 1;
        }
        vi->cx = 0;
        return 1;
    default:
        return 0;
    }
}

static void vi_normal_key(struct vi *vi, int c)
{
    if (c >= '0' && c <= '9') {
        if (c == '0' && vi->count == 0) {
            vi_move(vi, c);
            vi->last_key = c;
            return;
        }
        size_t digit = (size_t) (c - '0');
        if (vi->count > (VI_COUNT_MAX - digit) / 10)
            vi->count = VI_COUNT_MAX; /* clamp: no wrap, no multi-second loop */
        else
            vi->count = vi->count * 10 + digit;
        return;
    }

    if (vi_motion_key(vi, c)) {
        vi->last_key = c;
        return;
    }

    switch (c) {
    case 'g':
        if (vi->last_key == 'g') {
            if (vi->count > 0) {
                size_t target = vi->count - 1;
                if (target >= vi->buf.count)
                    target = vi->buf.count - 1;
                vi->cy = target;
                vi->count = 0;
            } else {
                vi->cy = 0;
            }
            vi->cx = 0;
            vi->last_key = 0;
            return;
        }
        break;
    case 7:
        vi_get_and_reset_count(vi);
        snprintf(vi->status, sizeof(vi->status), "\"%s\" %u lines%s",
                 vi->path[0] != '\0' ? vi->path : "[No Name]",
                 (unsigned) vi->buf.count, vi->dirty ? " modified" : "");
        vi_request_redraw(vi, VI_REDRAW_STATUS);
        break;
    case 'v':
    case 'V':
        vi_get_and_reset_count(vi);
        vi->visual = c == 'V' ? 2 : 1;
        vi->vsel_cx = vi->cx;
        vi->vsel_cy = vi->cy;
        vi_request_redraw(vi, VI_REDRAW_SCREEN);
        break;
    case 'i':
        vi_get_and_reset_count(vi);
        vi->insert = 1;
        vi_request_redraw(vi, VI_REDRAW_STATUS);
        break;
    case 'I':
        vi_get_and_reset_count(vi);
        vi_first_nonblank(vi);
        vi->insert = 1;
        vi_request_redraw(vi, VI_REDRAW_STATUS);
        break;
    case 'a':
        vi_get_and_reset_count(vi);
        if (vi->cx < vi->buf.line[vi->cy].len)
            vi->cx++;
        vi->insert = 1;
        vi_request_redraw(vi, VI_REDRAW_STATUS);
        break;
    case 'A':
        vi_get_and_reset_count(vi);
        vi->cx = vi->buf.line[vi->cy].len;
        vi->insert = 1;
        vi_request_redraw(vi, VI_REDRAW_STATUS);
        break;
    case 'o':
        vi_get_and_reset_count(vi);
        if (vi_insert_line(vi, vi->cy + 1, "", 0) == 0) {
            vi->cy++;
            vi->cx = 0;
            vi->insert = 1;
            vi->dirty = 1;
            vi_request_redraw(vi, VI_REDRAW_SCREEN);
        }
        break;
    case 'O':
        vi_get_and_reset_count(vi);
        if (vi_insert_line(vi, vi->cy, "", 0) == 0) {
            vi->cx = 0;
            vi->insert = 1;
            vi->dirty = 1;
            vi_request_redraw(vi, VI_REDRAW_SCREEN);
        }
        break;
    case 'x':
    case VI_KEY_DELETE: {
        size_t count = vi_get_and_reset_count(vi);
        struct vi_line *line = &vi->buf.line[vi->cy];
        if (vi->cx < line->len) {
            size_t span = line->len - vi->cx;
            if (count < span)
                span = count;
            vi_yank_text(vi, &line->data[vi->cx], span, 0);
            for (size_t i = 0; i < span; i++)
                vi_delete_char(vi);
        } else {
            for (size_t i = 0; i < count; i++)
                vi_delete_char(vi);
        }
        break;
    }
    case 'X': {
        size_t count = vi_get_and_reset_count(vi);
        struct vi_line *line = &vi->buf.line[vi->cy];
        if (vi->cx > 0) {
            size_t span = vi->cx;
            if (count < span)
                span = count;
            vi->cx -= span;
            vi_yank_text(vi, &line->data[vi->cx], span, 0);
            for (size_t i = 0; i < span; i++)
                vi_delete_char(vi);
        }
        break;
    }
    case '~': {
        size_t count = vi_get_and_reset_count(vi);
        struct vi_line *line = &vi->buf.line[vi->cy];
        for (size_t i = 0; i < count && vi->cx < line->len; i++) {
            char ch = line->data[vi->cx];
            if (islower((unsigned char) ch))
                line->data[vi->cx] = (char) toupper((unsigned char) ch);
            else if (isupper((unsigned char) ch))
                line->data[vi->cx] = (char) tolower((unsigned char) ch);
            if (vi->cx + 1 < line->len) {
                vi->cx++;
            } else {
                break;
            }
        }
        vi->dirty = 1;
        vi_request_redraw(vi, VI_REDRAW_LINE);
        break;
    }
    case 'D': {
        vi_get_and_reset_count(vi);
        struct vi_line *line = &vi->buf.line[vi->cy];
        if (vi->cx < line->len) {
            vi_yank_text(vi, line->data + vi->cx, line->len - vi->cx, 0);
            line->len = vi->cx;
            line->data[vi->cx] = '\0';
            vi->dirty = 1;
            vi_request_redraw(vi, VI_REDRAW_LINE);
        }
        break;
    }
    case 'C': {
        vi_get_and_reset_count(vi);
        struct vi_line *line = &vi->buf.line[vi->cy];
        if (vi->cx < line->len) {
            vi_yank_text(vi, line->data + vi->cx, line->len - vi->cx, 0);
            line->len = vi->cx;
            line->data[vi->cx] = '\0';
            vi->dirty = 1;
            vi_request_redraw(vi, VI_REDRAW_LINE);
        }
        vi->insert = 1;
        vi_request_redraw(vi, VI_REDRAW_STATUS);
        break;
    }
    case 'r': {
        int c2 = vi_read_key();
        if (c2 == VI_KEY_NULL)
            break;
        if (c2 == VI_KEY_EOF) {
            vi->quit = 1;
            break;
        }
        if (c2 != VI_KEY_ESC && c2 >= 32 && c2 <= 126) {
            struct vi_line *line = &vi->buf.line[vi->cy];
            size_t count = vi_get_and_reset_count(vi);
            for (size_t i = 0; i < count && vi->cx < line->len; i++) {
                line->data[vi->cx] = (char) c2;
                if (i + 1 < count && vi->cx + 1 < line->len) {
                    vi->cx++;
                }
            }
            vi->dirty = 1;
            vi_request_redraw(vi, VI_REDRAW_LINE);
        } else {
            vi_get_and_reset_count(vi);
        }
        break;
    }
    case 'p': {
        size_t count = vi_get_and_reset_count(vi);
        for (size_t i = 0; i < count; i++)
            vi_paste(vi, 1);
        break;
    }
    case 'P': {
        size_t count = vi_get_and_reset_count(vi);
        for (size_t i = 0; i < count; i++)
            vi_paste(vi, 0);
        break;
    }
    case 'J': {
        size_t count = vi_get_and_reset_count(vi);
        for (size_t i = 0; i < count; i++) {
            vi_join_line(vi);
        }
        break;
    }
    case 'd':
        if (vi->last_key == 'd') {
            size_t count = vi_get_and_reset_count(vi);
            vi_yank_line(vi, count);
            size_t deleted = 0;
            for (size_t i = 0; i < count; i++) {
                if (vi_delete_line(vi, vi->cy) == 0) {
                    deleted++;
                } else {
                    break;
                }
            }
            if (deleted > 0) {
                vi->dirty = 1;
                char msg[80];
                snprintf(msg, sizeof(msg), "%u line%s deleted",
                         (unsigned) deleted, deleted > 1 ? "s" : "");
                vi_status(vi, msg);
            }
            vi->last_key = 0;
            return;
        }
        break;
    case 'y':
        if (vi->last_key == 'y') {
            size_t count = vi_get_and_reset_count(vi);
            vi_yank_line(vi, count);
            vi->last_key = 0;
            return;
        }
        break;
    case '/':
        vi_get_and_reset_count(vi);
        vi_search(vi, 1);
        break;
    case '?':
        vi_get_and_reset_count(vi);
        vi_search(vi, 0);
        break;
    case 'n':
        vi_get_and_reset_count(vi);
        vi_search_step(vi, !vi->search_backward);
        break;
    case 'N':
        vi_get_and_reset_count(vi);
        vi_search_step(vi, vi->search_backward);
        break;
    case ':':
        vi_get_and_reset_count(vi);
        vi_ex(vi);
        break;
    case 19:  // Ctrl+S
        vi_get_and_reset_count(vi);
        vi_save(vi);
        break;
    case 17:  // Ctrl+Q
        vi_get_and_reset_count(vi);
        if (vi->dirty) {
            vi_status(vi, "No write since last change (use :q! to discard)");
        } else {
            vi->quit = 1;
        }
        break;
    default:
        vi_get_and_reset_count(vi);
        break;
    }
    vi->last_key = c;
}

/* Order the visual anchor and the cursor into an inclusive (start <= end) range
 * in (row, col) order.
 */
static void vi_visual_range(struct vi *vi,
                            size_t *sy,
                            size_t *sx,
                            size_t *ey,
                            size_t *ex)
{
    size_t ay = vi->vsel_cy, ax = vi->vsel_cx;
    size_t by = vi->cy, bx = vi->cx;

    if (ay < by || (ay == by && ax <= bx)) {
        *sy = ay, *sx = ax, *ey = by, *ex = bx;
    } else {
        *sy = by, *sx = bx, *ey = ay, *ex = ax;
    }
}

/* True if character index i on filerow lies inside the current selection. */
static int vi_pos_selected(struct vi *vi, size_t filerow, size_t i)
{
    size_t sy, sx, ey, ex;

    vi_visual_range(vi, &sy, &sx, &ey, &ex);
    if (filerow < sy || filerow > ey)
        return 0;
    if (vi->visual == 2) /* linewise: whole rows */
        return 1;
    if (sy == ey)
        return i >= sx && i <= ex;
    if (filerow == sy)
        return i >= sx;
    if (filerow == ey)
        return i <= ex;
    return 1;
}

/* Copy the selected span into the yank register without moving the cursor, so a
 * following delete still sees the original selection range. Charwise spans join
 * rows with newlines; linewise mirrors vi_yank_line's per-line format.
 */
static void vi_visual_yank(struct vi *vi)
{
    size_t sy, sx, ey, ex;

    vi_visual_range(vi, &sy, &sx, &ey, &ex);

    if (vi->visual == 2) {
        size_t save_cy = vi->cy;
        vi->cy = sy;
        vi_yank_line(vi, ey - sy + 1);
        vi->cy = save_cy;
        return;
    }

    /* Charwise: build the inclusive span [sy,sx]..[ey,ex]. */
    size_t total = 0;
    for (size_t r = sy; r <= ey; r++) {
        struct vi_line *ln = &vi->buf.line[r];
        size_t from = r == sy ? sx : 0;
        size_t to = r == ey ? (ex + 1) : ln->len; /* exclusive upper */
        if (from > ln->len)
            from = ln->len;
        if (to > ln->len)
            to = ln->len;
        size_t seg = to > from ? to - from : 0;
        size_t extra = r != ey ? 1 : 0; /* newline between rows */
        if (total > SIZE_MAX - seg - extra - 1)
            return; /* selection too large to hold in one buffer */
        total += seg + extra;
    }

    char *buf = malloc(total + 1);
    if (!buf)
        return;
    size_t pos = 0;
    for (size_t r = sy; r <= ey; r++) {
        struct vi_line *ln = &vi->buf.line[r];
        size_t from = r == sy ? sx : 0;
        size_t to = r == ey ? (ex + 1) : ln->len;
        if (from > ln->len)
            from = ln->len;
        if (to > ln->len)
            to = ln->len;
        if (to > from) {
            memcpy(buf + pos, ln->data + from, to - from);
            pos += to - from;
        }
        if (r != ey)
            buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    vi_yank_text(vi, buf, pos, 0);
    free(buf);
}

/* Move the cursor to the start of the current selection (post-yank vim rule).
 */
static void vi_visual_cursor_to_start(struct vi *vi)
{
    size_t sy, sx, ey, ex;

    vi_visual_range(vi, &sy, &sx, &ey, &ex);
    vi->cy = sy;
    vi->cx = vi->visual == 2 ? 0 : sx;
    if (vi->cx > vi->buf.line[vi->cy].len)
        vi->cx = vi->buf.line[vi->cy].len;
}

/* Delete the selected span, leaving the cursor at its start. */
static void vi_visual_delete(struct vi *vi)
{
    size_t sy, sx, ey, ex;

    vi_visual_range(vi, &sy, &sx, &ey, &ex);

    if (vi->visual == 2) {
        size_t n = ey - sy + 1;
        for (size_t i = 0; i < n; i++) {
            if (vi_delete_line(vi, sy) != 0)
                break;
        }
        vi->cy = sy < vi->buf.count ? sy : vi->buf.count - 1;
        vi->cx = 0;
        vi->dirty = 1;
        return;
    }

    struct vi_line *ls = &vi->buf.line[sy];
    if (sy == ey) {
        if (sx > ls->len)
            sx = ls->len;
        size_t to = ex + 1 > ls->len ? ls->len : ex + 1;
        while (to > sx) {
            vi_line_delete_char(ls, sx);
            to--;
        }
    } else {
        struct vi_line *le = &vi->buf.line[ey];
        size_t tail_from = ex + 1 > le->len ? le->len : ex + 1;
        size_t head = sx > ls->len ? ls->len : sx;

        /* merged head-of-first + tail-of-last, then drop the rows between. Do
         * the fallible merge first: on OOM leave the buffer untouched rather
         * than delete the interior rows and orphan the tail.
         */
        char *merged = malloc(head + (le->len - tail_from) + 1);
        if (!merged)
            return;
        memcpy(merged, ls->data, head);
        memcpy(merged + head, le->data + tail_from, le->len - tail_from);
        if (vi_line_set(ls, merged, head + (le->len - tail_from)) != 0) {
            free(merged);
            return;
        }
        free(merged);
        for (size_t r = 0; r < ey - sy; r++) {
            if (vi_delete_line(vi, sy + 1) != 0)
                break;
        }
    }
    vi->cy = sy;
    vi->cx = sx > vi->buf.line[vi->cy].len ? vi->buf.line[vi->cy].len : sx;
    vi->dirty = 1;
}

static void vi_visual_key(struct vi *vi, int c)
{
    if (c >= '0' && c <= '9' && (c != '0' || vi->count > 0)) {
        size_t digit = (size_t) (c - '0');
        if (vi->count > (VI_COUNT_MAX - digit) / 10)
            vi->count = VI_COUNT_MAX;
        else
            vi->count = vi->count * 10 + digit;
        return;
    }

    switch (c) {
    case VI_KEY_ESC:
        vi->visual = 0;
        vi_request_redraw(vi, VI_REDRAW_SCREEN);
        return;
    case 'v':
    case 'V': {
        /* toggle off on same mode, switch otherwise */
        int want = c == 'V' ? 2 : 1;
        vi->visual = vi->visual == want ? 0 : want;
        vi_request_redraw(vi, VI_REDRAW_SCREEN);
        return;
    }
    case 'y':
        vi_visual_yank(vi);
        vi_visual_cursor_to_start(vi);
        vi->visual = 0;
        vi_request_redraw(vi, VI_REDRAW_SCREEN);
        return;
    case 'd':
    case 'x':
        vi_visual_yank(vi); /* cut = yank then delete */
        vi_visual_delete(vi);
        vi->visual = 0;
        vi_request_redraw(vi, VI_REDRAW_SCREEN);
        return;
    case 'c':
        vi_visual_yank(vi);
        vi_visual_delete(vi);
        vi->visual = 0;
        vi->insert = 1;
        vi_request_redraw(vi, VI_REDRAW_SCREEN);
        return;
    default:
        break;
    }

    if (vi_motion_key(vi, c))
        vi_request_redraw(vi, VI_REDRAW_SCREEN);
    else
        vi_get_and_reset_count(vi);
    vi->last_key = c;
}

static void vi_insert_key(struct vi *vi, int c)
{
    if (c == VI_KEY_ESC) {
        vi->insert = 0;
        if (vi->cx > 0)
            vi->cx--;
        vi_request_redraw(vi, VI_REDRAW_STATUS);
        return;
    }
    if (c == 19) {  // Ctrl+S
        vi_save(vi);
        return;
    }
    if (c == 17) {  // Ctrl+Q
        if (vi->dirty) {
            vi_status(vi, "No write since last change (use :q! to discard)");
        } else {
            vi->quit = 1;
        }
        return;
    }
    if (c == VI_KEY_BACKSPACE || c == '\b') {
        vi_backspace(vi);
    } else if (c == '\r' || c == '\n') {
        vi_insert_newline(vi);
    } else if (c == VI_KEY_DELETE) {
        vi_delete_char(vi);
    } else if (c == VI_KEY_ARROW_LEFT || c == VI_KEY_ARROW_RIGHT ||
               c == VI_KEY_ARROW_UP || c == VI_KEY_ARROW_DOWN ||
               c == VI_KEY_HOME || c == VI_KEY_END || c == VI_KEY_PAGE_UP ||
               c == VI_KEY_PAGE_DOWN) {
        vi_move(vi, c);
    } else if (c >= 0 && c <= UCHAR_MAX && isprint(c)) {
        vi_insert_char(vi, c);
    }
}

static int vi_run(struct vi *vi)
{
    vi->rows = 24;
    vi->cols = 80;
    if (vi_load(vi) != 0) {
        vi_buffer_free(&vi->buf);
        return 1;
    }
    vi_status(vi, "toolbox vi: :help for keys");
    vi_signal_target = vi;
    vi_install_signals();
    vi_enable_raw(vi);
    vi_write("\x1b[?1049h\x1b[2J\x1b[H");
    vi_detect_window_size(vi);
    while (!vi->quit) {
        int c;

        if (vi_winch_pending) {
            vi_winch_pending = 0;
            vi_detect_window_size(vi);
        }
        vi_refresh(vi);
        errno = 0;
        c = vi_read_key();
        if (c == VI_KEY_NULL)
            continue; /* NUL keystroke (Ctrl+@): ignore */
        if (c == VI_KEY_EOF) {
            if (errno == EINTR)
                continue; /* signal (SIGWINCH) interrupted read; retry */
            break;        /* stdin EOF/hangup: exit rather than loop */
        }
        if (vi->insert)
            vi_insert_key(vi, c);
        else if (vi->visual)
            vi_visual_key(vi, c);
        else
            vi_normal_key(vi, c);
    }
    vi_disable_raw(vi);
    vi_write("\x1b[?1049l\x1b[?25h");
    vi_flush();
    vi_signal_target = NULL;
    vi_buffer_free(&vi->buf);
    free(vi->yank_buf);
    free(vi->hl_buf);
    free(vi_out_buf);
    vi_out_buf = NULL;
    vi_out_cap = 0;
    return 0;
}

int toolbox_vi_main(int argc, char **argv)
{
    struct vi vi;

    if (argc > 2) {
        fprintf(stderr, "usage: vi [FILE]\n");
        return 1;
    }
    memset(&vi, 0, sizeof(vi));
    if (argc == 2)
        snprintf(vi.path, sizeof(vi.path), "%s", argv[1]);
    vi.number = 0;
    return vi_run(&vi);
}
