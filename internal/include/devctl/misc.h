/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Non-portable miscellaneous devctl definitions. */
#ifndef __DEVCTL_MISC_H_INCLUDED
#define __DEVCTL_MISC_H_INCLUDED

#ifndef _DEVCTL_H_INCLUDED
#include <devctl.h>
#endif

#define DEVCTL_MQ_GET_ATTR __DIOF(DEVCTL_CLASS_MISC, 1, struct mq_attr)

#endif
