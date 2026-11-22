/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SH_PARSE_H
#define SH_PARSE_H

#include <stdbool.h>

enum sh_node_type {
    SH_COMMAND,
    SH_GROUP,
    SH_FUNCTION,
    SH_BACKGROUND,
    SH_PIPELINE,
    SH_AND_OR,
    SH_IF,
    SH_FOR,
    SH_WHILE,
    SH_UNTIL,
    SH_CASE,
    SH_ARM
};

struct sh_node {
    enum sh_node_type type;
    char *text; /* unexpanded command, case word or arm patterns */
    char *redirects;
    char *heredoc; /* literal lines after the command, through its delimiter */
    char *source;  /* the command a pipeline child executes */
    char *definition; /* source retained for child-shell function state */
    bool on_success;  /* preceding operator in an AND-OR list is && */
    unsigned refs;
    struct sh_node *condition, *body, *otherwise, *next;
};

/* Parse a command list, retaining shell words in their original spelling.
 * On success *out owns the list. */
bool sh_parse_list(const char *text, struct sh_node **out);
struct sh_node *sh_nodes_ref(struct sh_node *node);
void sh_nodes_free(struct sh_node *node);
#endif
