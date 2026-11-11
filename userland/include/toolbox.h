/*
 * toolbox shared applet interface.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TOOLBOX_H
#define TOOLBOX_H

struct toolbox_applet {
    const char *name;
    int (*main)(int, char **);
    const char *usage;
};

int toolbox_vi_main(int argc, char **argv);
int toolbox_sh_main(int argc, char **argv);
int toolbox_getty_main(int argc, char **argv);
int toolbox_mount_main(int argc, char **argv);
int toolbox_dmesg_main(int argc, char **argv);
const struct toolbox_applet *toolbox_applets(void);

#endif
