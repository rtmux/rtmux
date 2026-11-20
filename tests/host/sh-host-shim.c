/*
 * The toolbox shell as a host program, for tests/host/test-sh-host.py: the
 * applet table is empty and main runs the shell. sh_test_await_window is the
 * hook sh.c calls, when built with SH_TEST_AWAIT_WINDOW, between the point
 * where a read has found no trapped signal pending and the point where it waits
 * for input: the first time, with SH_TEST_RAISE set, it sends the shell the
 * signal there, the one moment a read could lose it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "toolbox.h"

#include <signal.h>
#include <stdlib.h>

static const struct toolbox_applet no_applets[] = {{NULL, NULL, NULL}};

const struct toolbox_applet *toolbox_applets(void)
{
    return no_applets;
}

void sh_test_await_window(void)
{
    static int calls;

    if (calls++ == 0 && getenv("SH_TEST_RAISE"))
        raise(SIGTERM);
}

int main(int argc, char **argv)
{
    return toolbox_sh_main(argc, argv);
}
