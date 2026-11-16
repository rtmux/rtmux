/*
 * toolbox sh - the one scanner for quoting and substitutions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sh-scan.h"

#include <string.h>

/* How deep substitutions may nest. Each level costs a few frames of recursion,
 * and a line is a few thousand characters, so without a cap a line of nothing
 * but "$(" would overrun a small stack. Past it, the rest of the line is taken
 * as the body of an unterminated one.
 */
#define SH_SCAN_MAX_DEPTH 32

static void unit_next(struct sh_scan *sc, struct sh_unit *u, int nest);

/* What scan_group knows of the command text it walks, to tell the parentheses
 * that close a $( ... ) from the ones that do not: a comment runs to the end of
 * its line, and in a case command the ) that ends a pattern (and the ( that may
 * open it) is not a closer.
 */
struct group {
    char word[8];    /* the plain word being read, if it is short enough */
    size_t wlen;     /* its length, or sizeof word once it is too long */
    bool cmd;        /* the next word is at command position */
    bool plain;      /* the word being read has only plain characters */
    int subject;     /* after `case`: 1 for its word, 2 when `in` is next */
    int ncase;       /* case commands open */
    unsigned pat;    /* bit per case, the innermost lowest: a pattern is next */
};

static bool word_is(const struct group *g, const char *kw)
{
    return g->plain && g->wlen == strlen(kw) && !memcmp(g->word, kw, g->wlen);
}

/* The word that just ended is complete. */
static void group_word(struct group *g)
{
    static const char *const lead[] = {"then", "do",    "else", "elif", "if",
                                       "while", "until", "!",    "{",    "}"};
    bool at_cmd = g->cmd;

    if (g->wlen == 0)
        return;
    g->cmd = false;
    if (g->subject == 1) {
        g->subject = 2;
    } else if (g->subject == 2 && word_is(g, "in")) {
        g->subject = 0;
        if (g->ncase < 31) {
            g->ncase++;
            g->pat = (g->pat << 1) | 1;
        }
    } else if (word_is(g, "esac") && g->ncase > 0 && ((g->pat & 1) || at_cmd)) {
        g->ncase--;
        g->pat >>= 1;
        g->cmd = true;
    } else if (at_cmd && word_is(g, "case")) {
        g->subject = 1;
    } else if (at_cmd) {
        for (size_t i = 0; i < sizeof lead / sizeof *lead; i++)
            if (word_is(g, lead[i]))
                g->cmd = true;
    }
    g->wlen = 0;
    g->plain = true;
}

/* The end of the text after an opening delimiter, scanned as unquoted command
 * text: the first character that closes it, where ')' balances the parentheses
 * opened inside. Sets *closed.
 */
static const char *scan_group(const char *p, char close, int nest, bool *closed)
{
    struct sh_scan sc = {p, 0};
    struct sh_unit u;
    struct group g = {.cmd = true, .plain = true};
    int depth = 1;

    *closed = false;
    if (nest > SH_SCAN_MAX_DEPTH)
        return p + strlen(p);
    for (;;) {
        const char *at = sc.p;
        bool pat;

        unit_next(&sc, &u, nest + 1);
        if (u.kind == SH_U_END)
            return at;
        if (u.kind != SH_U_LIT || sc.quote != 0) {
            g.plain = false; /* quoted or substituted: part of some word */
            g.wlen += g.wlen == 0;
            continue;
        }
        if (close != ')') {
            if (*at == close && --depth == 0) {
                *closed = true;
                return at;
            }
            continue;
        }
        switch (*at) {
        case ' ':
        case '\t':
        case '\n':
            group_word(&g);
            g.cmd = g.cmd || *at == '\n';
            break;
        case '#':
            if (g.wlen != 0)
                goto word;
            sc.p = strchr(at, '\n');
            if (!sc.p)
                sc.p = at + strlen(at);
            break;
        case ';':
            group_word(&g);
            g.cmd = true;
            if (at[1] == ';' && g.ncase > 0) {
                g.pat |= 1;
                sc.p = at + 2;
            }
            break;
        case '&':
        case '|':
            group_word(&g);
            g.cmd = true;
            break;
        case '(':
            group_word(&g);
            pat = g.ncase > 0 && (g.pat & 1);
            if (!pat)
                depth++;
            g.cmd = true;
            break;
        case ')':
            group_word(&g);
            pat = g.ncase > 0 && (g.pat & 1);
            g.cmd = true;
            if (pat) {
                g.pat &= ~1u;
            } else if (--depth == 0) {
                *closed = true;
                return at;
            }
            break;
        default:
        word:
            if (g.wlen < sizeof g.word)
                g.word[g.wlen] = *at;
            g.wlen++;
            break;
        }
    }
}

static void set_body(struct sh_unit *u,
                     enum sh_unit_kind kind,
                     const char *p,
                     size_t open,
                     const char *end,
                     bool closed,
                     size_t close)
{
    u->kind = kind;
    u->body = p + open;
    u->body_len = (size_t) (end - (p + open));
    u->closed = closed;
    u->len = (size_t) (end - p) + (closed ? close : 0);
}

static void unit_next(struct sh_scan *sc, struct sh_unit *u, int nest)
{
    const char *p = sc->p;
    const char *end;
    bool closed;

    u->len = 1;
    u->body = NULL;
    u->body_len = 0;
    u->closed = true;
    u->kind = SH_U_LIT;
    if (*p == '\0') {
        u->kind = SH_U_END;
        u->len = 0;
        return;
    }
    if (sc->quote == '\'') {
        if (*p == '\'') {
            u->kind = SH_U_QCLOSE;
            sc->quote = 0;
        }
    } else if (*p == '\\') {
        if (p[1] != '\0' && (sc->quote == 0 || strchr("$`\"\\", p[1]))) {
            u->kind = SH_U_ESC;
            u->len = 2;
        }
    } else if (*p == '\'' && sc->quote == 0) {
        u->kind = SH_U_QOPEN;
        sc->quote = '\'';
    } else if (*p == '"') {
        u->kind = sc->quote ? SH_U_QCLOSE : SH_U_QOPEN;
        sc->quote = sc->quote ? 0 : '"';
    } else if (*p == '`') {
        for (end = p + 1; *end != '\0' && *end != '`'; end++)
            if (*end == '\\' && end[1] != '\0')
                end++;
        set_body(u, SH_U_BQ, p, 1, end, *end == '`', 1);
    } else if (*p == '$' && p[1] == '(' && p[2] == '(') {
        int depth = 0;

        /* The expression ends at the ) that balances none opened inside it;
         * only parentheses count, as no quote can occur in one. A lone ) is
         * consumed, so a malformed one does not stall the caller.
         */
        for (end = p + 3; *end != '\0'; end++) {
            if (*end == '(')
                depth++;
            else if (*end == ')' && depth-- == 0)
                break;
        }
        closed = *end == ')' && end[1] == ')';
        set_body(u, SH_U_ARITH, p, 3, end, closed, 2);
        if (!closed && *end == ')')
            u->len++;
    } else if (*p == '$' && p[1] == '(') {
        end = scan_group(p + 2, ')', nest, &closed);
        set_body(u, SH_U_PAREN, p, 2, end, closed, 1);
    } else if (*p == '$' && p[1] == '{') {
        end = scan_group(p + 2, '}', nest, &closed);
        set_body(u, SH_U_BRACE, p, 2, end, closed, 1);
    } else if (*p == '$') {
        u->kind = SH_U_DOLLAR;
    }
    sc->p = p + u->len;
}

void sh_unit_next(struct sh_scan *sc, struct sh_unit *u)
{
    unit_next(sc, u, 0);
}

const char *sh_scan_find(const char *s, const char *set)
{
    struct sh_scan sc = {s, 0};
    struct sh_unit u;

    for (;;) {
        const char *at = sc.p;

        sh_unit_next(&sc, &u);
        if (u.kind == SH_U_END)
            return NULL;
        if (u.kind == SH_U_LIT && sc.quote == 0 && strchr(set, *at))
            return at;
    }
}
