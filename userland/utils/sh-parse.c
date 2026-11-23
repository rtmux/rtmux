/* SPDX-License-Identifier: Apache-2.0 */
#include "sh-parse.h"
#include "sh-scan.h"
#include <stdlib.h>
#include <string.h>

enum token_kind {
    END,
    WORD,
    SEMI,
    DSEMI,
    PIPE,
    RPAREN,
    LPAREN,
    AND_IF,
    OR_IF,
    OTHER
};
struct token {
    const char *begin, *end;
    enum token_kind kind;
};
struct parser {
    struct token token;
    const char *cursor;
    unsigned depth;
    bool failed;
    struct sh_node *pending_doc;
    struct sh_node *background_doc;
};

static char *span(struct parser *p, const char *start, const char *end);

/* Preserve the body when this command is serialized for a subshell. */
static void append_doc(struct parser *p, char **source, const char *body)
{
    size_t n = strlen(*source), m = strlen(body);
    char *joined = realloc(*source, n + m + 2);
    if (!joined) {
        p->failed = true;
        return;
    }
    joined[n] = '\n';
    memcpy(joined + n + 1, body, m + 1);
    *source = joined;
}

static void gather_doc(struct parser *p)
{
    struct sh_node *n = p->pending_doc;
    char *op = sh_heredoc_find(n->text);
    size_t cap = strlen(op) + 1;
    char *delim = malloc(cap);
    bool strip, quoted;
    p->pending_doc = NULL;
    if (!delim) {
        p->failed = true;
        return;
    }
    sh_heredoc_delim(op, delim, cap, &strip, &quoted);
    const char *begin = p->cursor, *line = begin;
    while (*line) {
        const char *end = line + strcspn(line, "\n");
        const char *word = strip ? line + strspn(line, "\t") : line;
        bool match = (size_t) (end - word) == strlen(delim) &&
                     !memcmp(word, delim, (size_t) (end - word));
        line = end + (*end != 0);
        if (match)
            break;
    }
    free(delim);
    n->heredoc = span(p, begin, line);
    if (n->heredoc && n->source)
        append_doc(p, &n->source, n->heredoc);
    if (n->heredoc && p->background_doc && p->background_doc->text)
        append_doc(p, &p->background_doc->text, n->heredoc);
    p->background_doc = NULL;
    p->cursor = line;
}

static void advance(struct parser *p)
{
    if (p->pending_doc && p->token.kind == SEMI && *p->token.begin == '\n')
        gather_doc(p);
    const char *s = p->cursor;
    for (;;) {
        s += strspn(s, " \t\r");
        if (*s != '#')
            break;
        s += strcspn(s, "\n");
    }
    p->token.begin = s;
    if (!*s)
        p->token.kind = END;
    else if (*s == ';' && s[1] == ';') {
        s += 2;
        p->token.kind = DSEMI;
    } else if (strchr(";\n|()&", *s)) {
        char op = *s++;
        p->token.kind = op == '|'   ? PIPE
                        : op == '(' ? LPAREN
                        : op == ')' ? RPAREN
                        : op == '&' ? OTHER
                                    : SEMI;
        if ((op == '&' || op == '|') && *s == op) {
            p->token.kind = op == '&' ? AND_IF : OR_IF;
            s++;
        }
    } else {
        struct sh_scan sc = {s, 0};
        struct sh_unit u;
        while (*sc.p && (sc.quote || !strchr(" \t\r\n;|()&", *sc.p) ||
                         (*sc.p == '&' && sc.p > s && sc.p[-1] == '>'))) {
            sh_unit_next(&sc, &u);
            if (!u.closed)
                p->failed = true;
            if (u.kind == SH_U_END)
                break;
        }
        if (sc.quote || sc.p == s)
            p->failed = true;
        s = sc.p;
        p->token.kind = WORD;
    }
    p->cursor = p->token.end = s;
}

static bool keyword(const struct parser *p, const char *word)
{
    size_t len = (size_t) (p->token.end - p->token.begin);
    return word && p->token.kind == WORD && strlen(word) == len &&
           !memcmp(p->token.begin, word, len);
}

static bool take(struct parser *p, const char *word)
{
    if (!keyword(p, word)) {
        p->failed = true;
        return false;
    }
    advance(p);
    return true;
}

static char *span(struct parser *p, const char *start, const char *end)
{
    size_t len = (size_t) (end - start);
    char *s = malloc(len + 1);
    if (!s) {
        p->failed = true;
        return NULL;
    }
    memcpy(s, start, len);
    s[len] = 0;
    return s;
}

struct sh_node *sh_nodes_ref(struct sh_node *n)
{
    if (n)
        n->refs++;
    return n;
}

void sh_nodes_free(struct sh_node *n)
{
    while (n) {
        if (--n->refs)
            return;
        struct sh_node *next = n->next;
        sh_nodes_free(n->condition);
        sh_nodes_free(n->body);
        sh_nodes_free(n->otherwise);
        free(n->heredoc);
        free(n->source);
        free(n->definition);
        free(n->redirects);
        free(n->text);
        free(n);
        n = next;
    }
}

static struct sh_node *node(struct parser *p, enum sh_node_type type)
{
    struct sh_node *n = calloc(1, sizeof(*n));
    if (n) {
        n->refs = 1;
        n->type = type;
    } else
        p->failed = true;
    return n;
}

static struct sh_node *list(struct parser *p,
                            const char *stop1,
                            const char *stop2,
                            const char *stop3);

static void newlines(struct parser *p)
{
    while (p->token.kind == SEMI && *p->token.begin == '\n')
        advance(p);
}

static void parse_case(struct parser *p, struct sh_node *n)
{
    advance(p);
    if (p->token.kind != WORD) {
        p->failed = true;
        return;
    }
    n->text = span(p, p->token.begin, p->token.end);
    advance(p);
    newlines(p);
    if (!take(p, "in"))
        return;
    struct sh_node **tail = &n->body;
    newlines(p);
    while (!p->failed && !keyword(p, "esac")) {
        if (p->token.kind == LPAREN)
            advance(p);
        const char *start = p->token.begin;
        while (p->token.kind == WORD || p->token.kind == PIPE)
            advance(p);
        if (p->token.kind != RPAREN || start == p->token.begin) {
            p->failed = true;
            break;
        }
        struct sh_node *arm = *tail = node(p, SH_ARM);
        if (!arm)
            break;
        arm->text = span(p, start, p->token.begin);
        advance(p);
        arm->body = list(p, "esac", NULL, NULL);
        tail = &arm->next;
        if (p->token.kind == DSEMI)
            advance(p);
        newlines(p);
    }
    take(p, "esac");
}

static void conditional(struct parser *p, struct sh_node *n)
{
    advance(p);
    n->condition = list(p, "then", NULL, NULL);
    if (!take(p, "then"))
        return;
    n->body = list(p, "else", "elif", "fi");
    if (!n->condition || !n->body)
        p->failed = true;
    if (keyword(p, "elif")) {
        n->otherwise = node(p, SH_IF);
        p->depth++;
        if (n->otherwise && p->depth <= 64)
            conditional(p, n->otherwise);
        else
            p->failed = true;
        p->depth--;
        return;
    }
    if (keyword(p, "else")) {
        advance(p);
        n->otherwise = list(p, "fi", NULL, NULL);
        if (!n->otherwise)
            p->failed = true;
    }
    take(p, "fi");
}

/* Retain the spelling of the redirection list. Its operands expand when the
 * compound command actually runs, including on successive loop passes.
 */
static void redirects(struct parser *p, struct sh_node *n)
{
    const char *start = p->token.begin, *end = start;

    while (p->token.kind == WORD) {
        const char *op = p->token.begin;
        if (*op >= '0' && *op <= '2')
            op++;
        if (*op != '<' && *op != '>')
            break;
        char first = *op++;
        if (*op == first || *op == '&')
            op++;
        bool separate = op == p->token.end;
        end = p->token.end;
        advance(p);
        if (separate) {
            if (p->token.kind != WORD) {
                p->failed = true;
                return;
            }
            end = p->token.end;
            advance(p);
        }
    }
    if (end > start)
        n->redirects = span(p, start, end);
}

static struct sh_node *command(struct parser *p)
{
    if (keyword(p, "then") || keyword(p, "fi") || keyword(p, "else") ||
        keyword(p, "elif") || keyword(p, "do") || keyword(p, "done") ||
        keyword(p, "esac") || keyword(p, "in") || keyword(p, "}")) {
        p->failed = true;
        return NULL;
    }
    enum sh_node_type type = SH_COMMAND;
    if (keyword(p, "case"))
        type = SH_CASE;
    else if (keyword(p, "if"))
        type = SH_IF;
    else if (keyword(p, "for"))
        type = SH_FOR;
    else if (keyword(p, "while"))
        type = SH_WHILE;
    else if (keyword(p, "until"))
        type = SH_UNTIL;
    else if (keyword(p, "{"))
        type = SH_GROUP;
    struct sh_node *n = node(p, type);
    if (!n)
        return NULL;
    switch (type) {
    case SH_IF:
        conditional(p, n);
        break;
    case SH_FOR: {
        const char *start = p->token.begin;
        advance(p);
        if (p->token.kind != WORD) {
            p->failed = true;
            break;
        }
        const char *end = p->token.end;
        advance(p);
        if (p->token.kind == SEMI)
            advance(p);
        newlines(p);
        if (keyword(p, "in")) {
            /* Treat do as data until the list of words has ended. */
            const char *in = p->token.begin;
            do {
                end = p->token.end;
                advance(p);
            } while (p->token.kind == WORD);

            /* A line break before "in" is a separator in the grammar, but
             * whitespace in the header passed to word expansion.
             */
            n->text = span(p, start, end);
            if (n->text)
                for (size_t i = 0; i < (size_t) (in - start); i++)
                    if (n->text[i] == '\n' || n->text[i] == ';')
                        n->text[i] = ' ';
            if (p->token.kind == SEMI)
                advance(p);
            newlines(p);
        } else {
            n->text = span(p, start, end);
        }
        if (take(p, "do")) {
            n->body = list(p, "done", NULL, NULL);
            if (!n->body)
                p->failed = true;
            take(p, "done");
        }
        break;
    }
    case SH_WHILE:
    case SH_UNTIL:
        advance(p);
        n->condition = list(p, "do", NULL, NULL);
        if (!n->condition)
            p->failed = true;
        if (take(p, "do")) {
            n->body = list(p, "done", NULL, NULL);
            if (!n->body)
                p->failed = true;
            take(p, "done");
        }
        break;
    case SH_CASE:
        parse_case(p, n);
        break;
    case SH_GROUP:
        advance(p);
        n->body = list(p, "}", NULL, NULL);
        if (!n->body)
            p->failed = true;
        take(p, "}");
        break;
    default: {
        const char *start = p->token.begin, *end = start;
        if (p->token.kind == WORD) {
            size_t len = (size_t) (p->token.end - start);
            struct parser look = *p;
            advance(&look);
            if (look.token.kind == LPAREN &&
                ((*start >= 'a' && *start <= 'z') ||
                 (*start >= 'A' && *start <= 'Z') || *start == '_') &&
                strspn(start,
                       "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ012"
                       "3456789_") == len) {
                n->text = span(p, start, p->token.end);
                if (!n->text)
                    break;
                *p = look;
                advance(p);
                if (p->token.kind != RPAREN) {
                    p->failed = true;
                    break;
                }
                advance(p);
                newlines(p);
                const char *body = p->token.begin;
                if (!keyword(p, "{")) {
                    p->failed = true;
                    break;
                }
                n->type = SH_FUNCTION;
                n->body = command(p);
                n->definition = span(p, body, p->token.begin);
                break;
            }
        }
        while (!p->failed && p->token.kind == WORD) {
            end = p->token.end;
            advance(p);
        }
        if (end == start)
            p->failed = true;
        n->text = span(p, start, end);
        if (n->text && sh_heredoc_find(n->text)) {
            if (p->pending_doc || sh_heredoc_find(sh_heredoc_find(n->text) + 2))
                p->failed = true;
            else
                p->pending_doc = n;
        }
        break;
    }
    }
    if (!p->failed && type != SH_COMMAND)
        redirects(p, n);
    return n;
}

/* A pipeline of simple commands still uses the job-control pipeline runner.
 * Keep its source intact so expansion and descriptor setup see all stages.
 */
static struct sh_node *pipeline(struct parser *p)
{
    const char *start = p->token.begin;
    struct sh_node *first = command(p);
    if (!first || p->failed || p->token.kind != PIPE)
        return first;
    struct sh_node *pipe = node(p, SH_PIPELINE);
    if (!pipe)
        return first;
    pipe->body = first;
    struct sh_node *stage = first;
    const char *from = start;
    bool simple = true;
    for (;;) {
        stage->source = span(p, from, p->token.begin);
        simple = simple && stage->type == SH_COMMAND && !stage->heredoc &&
                 p->pending_doc != stage;
        if (p->failed || p->token.kind != PIPE)
            break;
        advance(p);
        while (p->token.kind == SEMI && *p->token.begin == '\n')
            advance(p);
        from = p->token.begin;
        stage->next = command(p);
        if (!stage->next)
            break;
        stage = stage->next;
    }
    if (simple && !p->failed) {
        pipe->type = SH_COMMAND;
        pipe->text = span(p, start, p->token.begin);
        sh_nodes_free(pipe->body);
        pipe->body = NULL;
    }
    return pipe;
}

static struct sh_node *list(struct parser *p,
                            const char *a,
                            const char *b,
                            const char *c)
{
    struct sh_node *head = NULL, **tail = &head;
    if (++p->depth > 64)
        p->failed = true;
    newlines(p);
    while (!p->failed && p->token.kind != END && p->token.kind != DSEMI &&
           !keyword(p, a) && !keyword(p, b) && !keyword(p, c)) {
        const char *start = p->token.begin;
        struct sh_node *prior_doc = p->pending_doc;
        *tail = pipeline(p);
        if (!p->failed && (p->token.kind == AND_IF || p->token.kind == OR_IF)) {
            struct sh_node *group = node(p, SH_AND_OR);
            if (!group)
                break;
            struct sh_node *part = group->body = *tail;
            *tail = group;
            while (!p->failed &&
                   (p->token.kind == AND_IF || p->token.kind == OR_IF)) {
                bool on_success = p->token.kind == AND_IF;
                advance(p);
                while (p->token.kind == SEMI && *p->token.begin == '\n')
                    advance(p);
                part->next = pipeline(p);
                if (!part->next)
                    break;
                part = part->next;
                part->on_success = on_success;
            }
        }
        if (!p->failed && p->token.kind == OTHER) {
            struct sh_node *bg = node(p, SH_BACKGROUND);
            if (!bg)
                break;
            bg->body = *tail;
            bg->text = span(p, start, p->token.begin);
            if (p->pending_doc && p->pending_doc != prior_doc)
                p->background_doc = bg;
            *tail = bg;
            tail = &bg->next;
            advance(p);
            newlines(p);
            continue;
        }
        if (*tail)
            tail = &(*tail)->next;
        if (p->token.kind != SEMI && p->token.kind != DSEMI &&
            p->token.kind != END)
            p->failed = true;
        if (p->token.kind == SEMI)
            advance(p);
        newlines(p);
    }
    p->depth--;
    return head;
}

bool sh_parse_list(const char *text, struct sh_node **out)
{
    struct parser p = {.cursor = text};
    advance(&p);
    *out = list(&p, NULL, NULL, NULL);
    if (!p.failed && p.pending_doc)
        gather_doc(&p);
    if (p.failed || p.token.kind != END) {
        sh_nodes_free(*out);
        *out = NULL;
        return false;
    }
    return true;
}
