/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \file stdbool.h
 *
 * \brief Definition of the bool type.
 */

#ifndef _STDBOOL_H
#define _STDBOOL_H

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L
#error C99 or newer required
#endif

#define bool _Bool
#define false 0
#define true 1

#endif
