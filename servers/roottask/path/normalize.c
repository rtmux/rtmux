/*
 * RTMux namespace pathname normalization.
 *
 * This pure helper is deliberately host-buildable. It gives tests a seam for
 * pathname policy without booting roottask or constructing live namespace
 * nodes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include "path-normalize.h"

static int append_component(char *out,
                            size_t outsz,
                            size_t *len,
                            const char *component,
                            size_t component_len,
                            int absolute)
{
    size_t need = component_len;

    if (*len > (absolute ? 1U : 0U))
        need++;
    if (*len + need >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (*len > (absolute ? 1U : 0U))
        out[(*len)++] = '/';
    memcpy(out + *len, component, component_len);
    *len += component_len;
    out[*len] = '\0';
    return 0;
}

/* Returns 1 if a directory component was popped, 0 if the tail is unpoppable
 * (empty, root, or a literal ".."). Caller preserves a literal ".." in the 0
 * case so "../foo" stays "../foo" and "../.." stays "../..". Absolute paths
 * refuse to pop above "/".
 */
static int pop_component(char *out, size_t *len, int absolute)
{
    size_t root_len = absolute ? 1U : 0U;
    size_t tail_start;

    if (*len <= root_len)
        return 0;
    /* Locate the trailing component (everything after the last '/'). */
    tail_start = *len;
    while (tail_start > root_len && out[tail_start - 1] != '/')
        tail_start--;

    /* If the tail is itself "..", a parent traversal cannot cancel it — the
     * resulting path must keep climbing. Caller will append another "..".
     */
    if (*len - tail_start == 2 && out[tail_start] == '.' &&
        out[tail_start + 1] == '.')
        return 0;
    *len = tail_start;
    if (*len > root_len && out[*len - 1] == '/')
        (*len)--;
    out[*len] = '\0';
    return 1;
}

int path_normalize(const char *path, char *out, size_t outsz, int *dir_out)
{
    int absolute;
    int trailing_dir = 0;
    int dir_required = 0;
    size_t len;
    const char *p;

    if (!path || !out || outsz == 0) {
        errno = EINVAL;
        return -1;
    }

    absolute = path[0] == '/';
    len = absolute ? 1U : 0U;
    if (len >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    out[0] = absolute ? '/' : '\0';
    out[len] = '\0';

    p = path;
    while (*p != '\0') {
        const char *start;
        size_t component_len;

        while (*p == '/') {
            trailing_dir = 1;
            p++;
        }
        if (*p == '\0')
            break;

        start = p;
        while (*p != '\0' && *p != '/')
            p++;
        component_len = (size_t) (p - start);
        trailing_dir = *p == '/';

        if (component_len == 1 && start[0] == '.') {
            if (*p == '\0')
                dir_required = 1;
            continue;
        }
        if (component_len == 2 && start[0] == '.' && start[1] == '.') {
            if (*p == '\0')
                dir_required = 1;
            if (pop_component(out, &len, absolute))
                continue;

            /* Nothing to pop — for absolute paths ".." at root is just root
             * (POSIX: "/" is its own parent), so swallow it. For relative paths
             * we must preserve the literal ".." so that "../foo" does not
             * collapse to "foo".
             */
            if (absolute)
                continue;
            if (append_component(out, outsz, &len, "..", 2, absolute) != 0)
                return -1;
            continue;
        }
        if (append_component(out, outsz, &len, start, component_len,
                             absolute) != 0)
            return -1;
    }

    if (!absolute && len == 0) {
        if (outsz < 2) {
            errno = ENAMETOOLONG;
            return -1;
        }
        out[0] = '.';
        out[1] = '\0';
    }
    if (dir_out)
        *dir_out = dir_required || trailing_dir || !strcmp(out, "/") ||
                   !strcmp(out, ".");
    return 0;
}
