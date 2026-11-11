/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LDSO_PATH_H
#define LDSO_PATH_H

const char *ldso_basename(const char *name);
int ldso_dirname(const char *path, char *bname);
int ldso_dirname_abs(const char *path, char *base);

#endif /* LDSO_PATH_H */
