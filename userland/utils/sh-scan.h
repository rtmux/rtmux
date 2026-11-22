/*
 * toolbox sh - the one scanner for quoting and substitutions.
 *
 * Every part of the shell that has to know where a quote, an escape or a
 * substitution starts and ends (the tokenizer, the statement splitter, the
 * compound-command and here-document finders, line continuation) reads the line
 * through sh_unit_next, so they cannot disagree. It has no dependency on the
 * rest of the shell and is also built for the host tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SH_SCAN_H
#define SH_SCAN_H

#include <stdbool.h>
#include <stddef.h>

char *sh_heredoc_find(char *line);
char *sh_heredoc_delim(char *hh, char *delim, size_t dsize, bool *strip, bool *quoted);
bool sh_command_prefix(const char *word, size_t len);

enum sh_unit_kind {
    SH_U_END,    /* end of the string */
    SH_U_LIT,    /* one character with no special meaning here */
    SH_U_QOPEN,  /* ' or " that opens a quote; the scanner is now inside it */
    SH_U_QCLOSE, /* the ' or " that closes the quote */
    SH_U_ESC,    /* a backslash and the character it makes literal */
    SH_U_PAREN,  /* $( ... ); body is the command text */
    SH_U_ARITH,  /* $(( ... )); body is the expression */
    SH_U_BQ,     /* ` ... `; body is the text, still escaped */
    SH_U_BRACE,  /* ${ ... }; body is the text between the braces */
    SH_U_DOLLAR, /* a $ that starts a plain parameter (or is literal) */
};

struct sh_scan {
    const char *p; /* the next character to read */
    int quote;     /* 0, '\'' or '"': the quote the scanner is inside */
};

struct sh_unit {
    enum sh_unit_kind kind;
    size_t len;       /* characters the unit takes, from sc->p */
    const char *body; /* PAREN, ARITH, BQ, BRACE: the text inside */
    size_t body_len;
    bool closed; /* the unit has its closing delimiter */
};

/* Read the unit at sc->p under sc->quote into *u and advance sc past it,
 * updating sc->quote on a QOPEN or QCLOSE. A substitution is one unit, however
 * much quoting and nesting it holds, and its text is scanned the same way
 * (sc->quote does not leak into or out of it). A backslash is literal in single
 * quotes, and in double quotes escapes only $ ` " and \ (POSIX 2.2).
 */
void sh_unit_next(struct sh_scan *sc, struct sh_unit *u);

/* The first character of s in set that is not quoted, escaped or inside a
 * substitution, or NULL.
 */
const char *sh_scan_find(const char *s, const char *set);

/* The end of the case command that opens at p, which points at its `case`: just
 * past its esac, however many case commands are nested in its arms, or NULL
 * when it is not closed.
 */
const char *sh_scan_case_end(const char *p);

#endif /* SH_SCAN_H */
